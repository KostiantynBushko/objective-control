#include "backends/backends_internal.hpp"

#ifdef OC_HAVE_OPENCV
#include "backends/http_mjpeg.hpp"
#endif

#ifdef OC_HAVE_FFMPEG
#include "backends/rtsp_ffmpeg.hpp"
#endif

#include <chrono>
#include <cstring>
#include <string>

#ifdef OC_HAVE_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif

namespace oc::detail {

namespace {

// True if @p url uses a real-time streaming protocol handled by OpenCV/FFmpeg.
bool isRtsp(const std::string& url) {
    return url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0;
}

#ifdef OC_HAVE_OPENCV
std::optional<Frame> matToFrame(const cv::Mat& image,
                             double timestamp_ms = 0.0);

// RTSP (or any FFmpeg-decoded stream) backend using cv::VideoCapture.
//
// This is the fallback for builds without FFmpeg. It cannot be used for a
// live preview: VideoCapture fills a queue with decoded frames and hands
// them out oldest first, so the picture ends up seconds behind the camera.
class RtspCamera final : public Camera {
public:
    ~RtspCamera() override { close(); }

    bool open(const CameraConfig& config) override {
        if (!capture_.open(config.source, cv::CAP_FFMPEG)) {
            if (!capture_.open(config.source)) {
                return false;
            }
        }
        live_ = config.live;
        opened_ = true;
        return true;
    }

    void close() override {
        if (opened_) {
            capture_.release();
            opened_ = false;
        }
    }

    bool isOpen() const override { return opened_; }

    std::optional<Frame> grab() override {
        cv::Mat image;
        if (live_) {
            if (!grabNewest(image)) {
                return std::nullopt;
            }
        } else if (!capture_.read(image) || image.empty()) {
            return std::nullopt;
        }
        return matToFrame(image, capture_.get(cv::CAP_PROP_POS_MSEC));
    }

private:
    /// Grabs the newest decoded frame and throws away the ones that piled
    /// up behind it.
    ///
    /// VideoCapture decodes into a queue and read() hands back the OLDEST
    /// frame in it. When the consumer is slower than the camera the queue
    /// only grows, so every frame that comes out is older than the last and
    /// the preview falls further and further behind.
    ///
    /// grab() advances the decoder and decodes one frame, retrieve()
    /// converts the frame that was decoded last. Grabbing repeatedly and
    /// retrieving once therefore skips over the queued frames and yields
    /// the newest one. How many frames to skip is decided by how long the
    /// grab took: one that returns at once means the decoder still had a
    /// frame queued, one that waits about a frame period means the queue
    /// was empty and the frame in hand is already the newest.
    bool grabNewest(cv::Mat& out) {
        bool decoded = false;
        for (int i = 0; i < kMaxSkip; ++i) {
            const auto start = std::chrono::steady_clock::now();
            if (!capture_.grab()) {
                if (!decoded) {
                    return false;
                }
                break;
            }
            decoded = true;
            const auto waited =
                std::chrono::steady_clock::now() - start;
            if (waited > kQueuedGrabWait) {
                // The decoder had to wait for the network, so there was
                // nothing left in the queue: this frame is the newest.
                break;
            }
        }
        return decoded && capture_.retrieve(out) && !out.empty();
    }

    /// A grab() that returns faster than this found a frame already queued.
    static constexpr auto kQueuedGrabWait = std::chrono::milliseconds(8);

    /// Upper bound on the frames skipped per grab, so a stalled decoder
    /// cannot keep the caller waiting forever.
    static constexpr int kMaxSkip = 60;

    cv::VideoCapture capture_;
    bool opened_ = false;
    bool live_ = false;
};

// HTTP MJPEG / snapshot backend using the self-contained client.
class HttpCamera final : public Camera {
public:
    ~HttpCamera() override { close(); }

    bool open(const CameraConfig& config) override {
        if (!client_.open(config.source)) {
            return false;
        }
        opened_ = true;
        return true;
    }

    void close() override {
        opened_ = false;
        client_.close();
    }

    bool isOpen() const override { return opened_; }

    std::optional<Frame> grab() override {
#ifdef OC_HAVE_OPENCV
        cv::Mat image;
        if (!client_.read(image)) {
            return std::nullopt;
        }
        return matToFrame(image);
#else
        (void)client_;
        return std::nullopt;
#endif
    }

private:
    HttpMjpegClient client_;
    bool opened_ = false;
};
#endif // OC_HAVE_OPENCV

#if defined(OC_HAVE_OPENCV) || defined(OC_HAVE_FFMPEG)
// Dispatches to RTSP (FFmpeg or OpenCV) or HTTP (self-contained) based on
// the URL.
class IpCamera final : public Camera {
public:
    ~IpCamera() override {
        if (impl_) {
            impl_->close();
        }
    }

    bool open(const CameraConfig& config) override {
        if (config.source.empty()) {
            return false;
        }
        if (isRtsp(config.source)) {
#ifdef OC_HAVE_FFMPEG
            // The FFmpeg backend decodes with low-delay settings and hands
            // out the newest frame, so a live preview is not seconds behind
            // the camera.
            impl_ = makeRtspCamera();
#elif defined(OC_HAVE_OPENCV)
            impl_ = std::make_unique<RtspCamera>();
#else
            return false;
#endif
        } else {
#ifdef OC_HAVE_OPENCV
            impl_ = std::make_unique<HttpCamera>();
#else
            return false;
#endif
        }
        return impl_->open(config);
    }

    void close() override {
        if (impl_) {
            impl_->close();
        }
    }

    bool isOpen() const override { return impl_ && impl_->isOpen(); }

    std::optional<Frame> grab() override {
        return impl_ ? impl_->grab() : std::nullopt;
    }

private:
    std::unique_ptr<Camera> impl_;
};
#endif // OC_HAVE_OPENCV || OC_HAVE_FFMPEG

#ifdef OC_HAVE_OPENCV
// Converts a BGR cv::Mat into a Frame (deep copy).
std::optional<Frame> matToFrame(const cv::Mat& image, double timestamp_ms) {
    Frame frame;
    frame.width = image.cols;
    frame.height = image.rows;
    frame.channels = image.channels();
    // The source presentation time, as the embedded backend reports it. It
    // is the only handle on how old a frame is, so it is worth keeping.
    frame.timestamp_us = static_cast<std::int64_t>(timestamp_ms) * 1000;

    const std::size_t bytes =
        static_cast<std::size_t>(image.total()) * image.elemSize();
    frame.data.resize(bytes);
    std::memcpy(frame.data.data(), image.data, bytes);
    return frame;
}
#endif

} // namespace

std::unique_ptr<Camera> makeIpCamera() {
#if defined(OC_HAVE_OPENCV) || defined(OC_HAVE_FFMPEG)
    return std::make_unique<IpCamera>();
#else
    return nullptr;
#endif
}

} // namespace oc::detail
