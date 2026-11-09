#include "backends/backends_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef OC_HAVE_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif

namespace oc::detail {

namespace {

// True if the whole string is decimal digits (i.e. a device index).
bool isDecimal(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

#ifdef OC_HAVE_OPENCV
class EmbeddedCamera final : public Camera {
public:
    ~EmbeddedCamera() override { close(); }

    bool open(const CameraConfig& config) override {
        bool opened = false;
        if (config.source.empty() || isDecimal(config.source)) {
            // Empty -> first device; numeric -> open by index.
            const int index = config.source.empty() ? 0 : std::stoi(config.source);
            opened = capture_.open(index);
        } else {
            // Non-numeric -> treat as a device path, e.g. "/dev/video0".
            opened = capture_.open(config.source);
        }

        if (!opened) {
            return false;
        }

        capture_.set(cv::CAP_PROP_FRAME_WIDTH, config.width);
        capture_.set(cv::CAP_PROP_FRAME_HEIGHT, config.height);
        capture_.set(cv::CAP_PROP_FPS, config.fps);

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

        Frame frame;
        frame.width = image.cols;
        frame.height = image.rows;
        frame.channels = image.channels();
        frame.timestamp_us = static_cast<std::int64_t>(capture_.get(cv::CAP_PROP_POS_MSEC)) * 1000;

        const std::size_t bytes = static_cast<std::size_t>(image.total()) * image.elemSize();
        frame.data.resize(bytes);
        std::memcpy(frame.data.data(), image.data, bytes);
        return frame;
    }

private:
    cv::VideoCapture capture_;
    bool opened_ = false;
};
#endif

} // namespace

std::unique_ptr<Camera> makeEmbeddedCamera() {
#ifdef OC_HAVE_OPENCV
    return std::make_unique<EmbeddedCamera>();
#else
    return nullptr;
#endif
}

} // namespace oc::detail
