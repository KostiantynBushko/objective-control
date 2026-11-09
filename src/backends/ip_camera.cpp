#include "backends/backends_internal.hpp"

#include "backends/http_mjpeg.hpp"

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
std::optional<Frame> matToFrame(const cv::Mat& image);

// RTSP (or any FFmpeg-decoded stream) backend using cv::VideoCapture.
class RtspCamera final : public Camera {
public:
    ~RtspCamera() override { close(); }

    bool open(const CameraConfig& config) override {
        if (!capture_.open(config.source, cv::CAP_FFMPEG)) {
            if (!capture_.open(config.source)) {
                return false;
            }
        }
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
        if (!capture_.read(image) || image.empty()) {
            return std::nullopt;
        }
        return matToFrame(image);
    }

private:
    cv::VideoCapture capture_;
    bool opened_ = false;
};
#endif

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

#ifdef OC_HAVE_OPENCV
// Dispatches to RTSP (OpenCV) or HTTP (self-contained) based on the URL.
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
            impl_ = std::make_unique<RtspCamera>();
        } else {
            impl_ = std::make_unique<HttpCamera>();
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

// Converts a BGR cv::Mat into a Frame (deep copy).
std::optional<Frame> matToFrame(const cv::Mat& image) {
    Frame frame;
    frame.width = image.cols;
    frame.height = image.rows;
    frame.channels = image.channels();
    frame.timestamp_us = 0;

    const std::size_t bytes =
        static_cast<std::size_t>(image.total()) * image.elemSize();
    frame.data.resize(bytes);
    std::memcpy(frame.data.data(), image.data, bytes);
    return frame;
}
#endif

} // namespace

std::unique_ptr<Camera> makeIpCamera() {
#ifdef OC_HAVE_OPENCV
    return std::make_unique<IpCamera>();
#else
    return nullptr;
#endif
}

} // namespace oc::detail
