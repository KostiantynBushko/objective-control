#include "backends/backends_internal.hpp"

#include "backends/rtsp_ffmpeg.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oc::detail {

namespace {

// How many frames a consumer that is not in live mode may fall behind
// before the oldest ones are dropped. A recording keeps every frame, but a
// consumer that stops reading must not make the backend grow forever.
constexpr std::size_t kMaxQueuedFrames = 120;

// Memory budget for the queue of frames waiting for a consumer. A frame of
// a 1080p camera is 6 MB, so this holds about ten of them.
constexpr std::size_t kMaxQueuedBytes = 64u * 1024u * 1024u;

// How long grab() waits for a frame before it reports that there is none.
constexpr auto kFrameTimeout = std::chrono::seconds(2);

// The log level FFmpeg uses while the backend is open.
//
// FFmpeg writes to stderr by default, and a camera that sends a slightly
// broken stream makes it complain about every single frame. The library
// therefore keeps quiet; set OBJECTIVE_CONTROL_FFMPEG_LOG to "error",
// "warning", "info" or "debug" to see what FFmpeg has to say.
int ffmpegLogLevel() {
    const char* requested = std::getenv("OBJECTIVE_CONTROL_FFMPEG_LOG");
    if (requested == nullptr) {
        return AV_LOG_FATAL;
    }
    std::string level(requested);
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (level == "quiet") {
        return AV_LOG_QUIET;
    }
    if (level == "error") {
        return AV_LOG_ERROR;
    }
    if (level == "warning" || level == "warn") {
        return AV_LOG_WARNING;
    }
    if (level == "info") {
        return AV_LOG_INFO;
    }
    if (level == "debug" || level == "trace") {
        return AV_LOG_DEBUG;
    }
    return AV_LOG_FATAL;
}

// RTSP camera on top of FFmpeg.
//
// OpenCV's VideoCapture cannot be used for a low-latency preview: it fills a
// queue with decoded frames and hands them out oldest first, and the
// decoder behind it is allowed to hold frames back for reordering. On a
// live stream that queue turns into a growing delay -- the picture on
// screen is the picture the camera sent seconds ago.
//
// This backend decodes with the settings ffplay uses for a live preview
// ("-rtsp_transport udp -fflags nobuffer -flags low_delay") and keeps only
// the newest frame, so the consumer always gets the current picture.
class FfmpegRtspCamera final : public Camera {
public:
    ~FfmpegRtspCamera() override { close(); }

    bool open(const CameraConfig& config) override {
        close();

        // FFmpeg logs to stderr by default, which would drown the output of
        // the application that uses the library.
        logLevel_ = av_log_get_level();
        av_log_set_level(ffmpegLogLevel());

        if (config.source.empty()) {
            return false;
        }
        live_ = config.live;

        // Set before the session is opened: the interrupt callback below
        // aborts the network operations as soon as this goes false.
        running_ = true;

        // UDP carries the lowest latency: a lost TCP segment stalls the
        // whole stream until it is retransmitted. Not every camera or
        // network allows it though, so TCP is the fallback.
        const std::string transport =
            config.transport.empty() ? std::string("udp") : config.transport;
        bool ok = false;
        if (transport == "udp") {
            ok = openStream(config.source, "udp") || openStream(config.source, "tcp");
        } else if (transport == "tcp") {
            ok = openStream(config.source, "tcp");
        } else {
            ok = openStream(config.source, transport.c_str());
        }
        if (!ok) {
            running_ = false;
            restoreLogLevel();
            return false;
        }

        opened_ = true;
        worker_ = std::thread([this] { decodeLoop(); });
        return true;
    }

    void close() override {
        running_ = false; // also aborts a blocking av_read_frame()
        if (worker_.joinable()) {
            worker_.join();
        }

        if (sws_ != nullptr) {
            sws_freeContext(sws_);
            sws_ = nullptr;
        }
        if (codecContext_ != nullptr) {
            avcodec_free_context(&codecContext_);
            codecContext_ = nullptr;
        }
        if (format_ != nullptr) {
            avformat_close_input(&format_);
            format_ = nullptr;
        }

        restoreLogLevel();

        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        queuedBytes_ = 0;
        pending_ = Frame{};
        published_ = 0;
        delivered_ = 0;
        opened_ = false;
    }

    bool isOpen() const override { return opened_; }

    std::optional<Frame> grab() override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (live_) {
            // Wait for a frame that is newer than the one handed out last.
            // The wait is bounded so that a camera that stopped sending
            // does not block the caller forever.
            const bool ready = frameReady_.wait_for(lock, kFrameTimeout, [this] {
                return published_ != delivered_ || !running_.load();
            });
            if (!ready || published_ == delivered_) {
                return std::nullopt; // no new frame (yet)
            }
            delivered_ = published_;
            return pending_;
        }

        const bool ready = frameReady_.wait_for(lock, kFrameTimeout, [this] {
            return !queue_.empty() || !running_.load();
        });
        if (!ready || queue_.empty()) {
            return std::nullopt;
        }
        Frame frame = std::move(queue_.front());
        queuedBytes_ -= frame.data.size();
        queue_.pop_front();
        return frame;
    }

private:
    // Opens the RTSP session and the decoder.
    bool openStream(const std::string& url, const char* transport) {
        AVDictionary* options = nullptr;
        av_dict_set(&options, "rtsp_transport", transport, 0);
        // Let the demuxer hand the packets over as they arrive: every
        // buffered packet is a frame the user gets to see later.
        av_dict_set(&options, "fflags", "nobuffer", 0);

        AVFormatContext* format = avformat_alloc_context();
        if (format == nullptr) {
            av_dict_free(&options);
            return false;
        }
        // Lets close() interrupt a blocking read.
        format->interrupt_callback.callback = &FfmpegRtspCamera::interruptCallback;
        format->interrupt_callback.opaque = this;

        int rc = avformat_open_input(&format, url.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (rc < 0) {
            return false; // avformat_open_input frees the context
        }

        rc = avformat_find_stream_info(format, nullptr);
        if (rc < 0) {
            avformat_close_input(&format);
            return false;
        }

        int videoStream = -1;
        for (unsigned int i = 0; i < format->nb_streams; ++i) {
            if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoStream = static_cast<int>(i);
                break;
            }
        }
        if (videoStream < 0) {
            avformat_close_input(&format);
            return false;
        }

        AVStream* stream = format->streams[videoStream];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr) {
            avformat_close_input(&format);
            return false;
        }

        AVCodecContext* codecContext = avcodec_alloc_context3(codec);
        if (codecContext == nullptr) {
            avformat_close_input(&format);
            return false;
        }
        avcodec_parameters_to_context(codecContext, stream->codecpar);
        // Low delay: hand every frame over as soon as it is decoded instead
        // of holding it back for reordering. This is ffplay's
        // "-flags low_delay" and it is what removes the seconds of delay.
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
        // Let FFmpeg pick the number of threads: the frames of a stream
        // without reordering are decoded in parallel, so the picture is not
        // held back while the other frames are being decoded.
        codecContext->thread_count = 0;

        rc = avcodec_open2(codecContext, codec, nullptr);
        if (rc < 0) {
            avcodec_free_context(&codecContext);
            avformat_close_input(&format);
            return false;
        }

        format_ = format;
        codecContext_ = codecContext;
        videoStream_ = videoStream;
        timeBase_ = av_q2d(stream->time_base);
        return true;
    }

    // Puts FFmpeg's log level back the way it was found.
    void restoreLogLevel() {
        if (logLevel_ >= 0) {
            av_log_set_level(logLevel_);
            logLevel_ = -1;
        }
    }

    // Called by FFmpeg while it waits for data. Returning non-zero aborts
    // the pending network operation.
    static int interruptCallback(void* opaque) {
        const auto* self = static_cast<const FfmpegRtspCamera*>(opaque);
        return (self != nullptr && self->running_.load()) ? 0 : 1;
    }

    // Reads packets, decodes them and publishes the frames.
    void decodeLoop() {
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        if (packet == nullptr || frame == nullptr) {
            av_frame_free(&frame);
            av_packet_free(&packet);
            running_ = false;
            return;
        }

        while (running_.load()) {
            const int rc = av_read_frame(format_, packet);
            if (rc < 0) {
                if (rc == AVERROR(EAGAIN) || rc == AVERROR(EWOULDBLOCK)) {
                    continue; // nothing yet, try again
                }
                break; // the stream ended or failed
            }
            if (packet->stream_index != videoStream_) {
                av_packet_unref(packet);
                continue;
            }
            if (avcodec_send_packet(codecContext_, packet) == 0) {
                while (avcodec_receive_frame(codecContext_, frame) == 0) {
                    publish(frame);
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(packet);
        }

        av_frame_free(&frame);
        av_packet_free(&packet);

        running_ = false;
        std::lock_guard<std::mutex> lock(mutex_);
        frameReady_.notify_all();
    }

    // Converts the decoded frame to BGR and hands it to the consumer.
    void publish(const AVFrame* frame) {
        if (frame->width <= 0 || frame->height <= 0) {
            return;
        }

        Frame out;
        out.width = frame->width;
        out.height = frame->height;
        out.channels = 3;
        out.timestamp_us = timestampOf(frame);
        out.data.resize(static_cast<std::size_t>(frame->width) *
                        static_cast<std::size_t>(frame->height) * 3u);
        if (!convert(frame, out.data)) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (live_) {
            // Only the newest frame matters: the ones that piled up behind
            // it are older than the picture the camera shows right now.
            pending_ = std::move(out);
        } else {
            queuedBytes_ += out.data.size();
            queue_.push_back(std::move(out));
            while (queue_.size() > kMaxQueuedFrames ||
                   queuedBytes_ > kMaxQueuedBytes) {
                queuedBytes_ -= queue_.front().data.size();
                queue_.pop_front();
            }
        }
        ++published_;
        frameReady_.notify_all();
    }

    // Converts the frame to the BGR layout the library uses.
    bool convert(const AVFrame* frame, std::vector<std::uint8_t>& out) {
        // A camera that sends full-range YUV uses the deprecated "YUVJ"
        // pixel formats. The range is told to swscale separately, so the
        // plain format is used here -- with the range set, the colours come
        // out right and swscale does not complain about the format.
        auto format = static_cast<AVPixelFormat>(frame->format);
        bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
        if (format == AV_PIX_FMT_YUVJ420P) {
            format = AV_PIX_FMT_YUV420P;
            fullRange = true;
        } else if (format == AV_PIX_FMT_YUVJ422P) {
            format = AV_PIX_FMT_YUV422P;
            fullRange = true;
        } else if (format == AV_PIX_FMT_YUVJ444P) {
            format = AV_PIX_FMT_YUV444P;
            fullRange = true;
        }

        if (sws_ == nullptr || swsWidth_ != frame->width ||
            swsHeight_ != frame->height || swsFormat_ != format) {
            if (sws_ != nullptr) {
                sws_freeContext(sws_);
                sws_ = nullptr;
            }
            sws_ = sws_getContext(frame->width, frame->height, format,
                                  frame->width, frame->height, AV_PIX_FMT_BGR24,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws_ == nullptr) {
                return false;
            }
            const int* coefficients =
                sws_getCoefficients(frame->colorspace == AVCOL_SPC_BT709
                                        ? SWS_CS_ITU709
                                        : SWS_CS_DEFAULT);
            if (coefficients != nullptr) {
                sws_setColorspaceDetails(sws_, coefficients, fullRange ? 1 : 0,
                                         coefficients, 0, 0, 1 << 16, 1 << 16);
            }
            swsWidth_ = frame->width;
            swsHeight_ = frame->height;
            swsFormat_ = format;
        }

        uint8_t* dstData[4] = {out.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {frame->width * 3, 0, 0, 0};
        return sws_scale(sws_, frame->data, frame->linesize, 0, frame->height,
                         dstData, dstStride) > 0;
    }

    // The presentation time of the frame, in microseconds. It is the time
    // the camera gave the frame, so a consumer can tell how old it is.
    std::int64_t timestampOf(const AVFrame* frame) const {
        if (frame->pts == AV_NOPTS_VALUE) {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }
        const double seconds = static_cast<double>(frame->pts) * timeBase_;
        return static_cast<std::int64_t>(seconds * 1000000.0);
    }

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codecContext_ = nullptr;
    int videoStream_ = -1;
    double timeBase_ = 0.0;

    SwsContext* sws_ = nullptr;
    int swsWidth_ = 0;
    int swsHeight_ = 0;
    AVPixelFormat swsFormat_ = AV_PIX_FMT_NONE;

    std::thread worker_;
    int logLevel_ = -1;
    std::atomic<bool> running_{false};
    bool opened_ = false;
    bool live_ = false;

    std::mutex mutex_;
    std::condition_variable frameReady_;
    std::deque<Frame> queue_;  // frames waiting for a consumer
    std::size_t queuedBytes_ = 0;
    Frame pending_;            // newest frame, used in live mode
    std::uint64_t published_ = 0;
    std::uint64_t delivered_ = 0;
};

} // namespace

std::unique_ptr<Camera> makeRtspCamera() {
    return std::make_unique<FfmpegRtspCamera>();
}

} // namespace oc::detail
