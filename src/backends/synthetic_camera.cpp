#include "backends/backends_internal.hpp"

#include <chrono>
#include <cstdlib>

namespace oc::detail {

namespace {

/// Synthesizes a live test pattern (a moving bright bar over a 2D gradient)
/// so the full Camera/Frame pipeline can be exercised without hardware.
class SyntheticCamera final : public Camera {
public:
    bool open(const CameraConfig& config) override {
        width_ = config.width > 0 ? config.width : 640;
        height_ = config.height > 0 ? config.height : 480;
        start_ = std::chrono::steady_clock::now();
        opened_ = true;
        return true;
    }

    void close() override { opened_ = false; }

    bool isOpen() const override { return opened_; }

    std::optional<Frame> grab() override {
        if (!opened_) {
            return std::nullopt;
        }
        return render(frame_index_++);
    }

private:
    Frame render(int n) const {
        constexpr int kChannels = 3;

        Frame f;
        f.width = width_;
        f.height = height_;
        f.channels = kChannels;
        f.timestamp_us = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_).count());
        f.data.resize(static_cast<std::size_t>(width_) * height_ * kChannels);

        const int band = (n * 16) % width_; // moving bright-bar column

        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(y) * width_ + x) * kChannels;
                // BGR order, matching the other backends.
                f.data[i + 0] = static_cast<std::uint8_t>((x * 255) / width_);
                f.data[i + 1] = static_cast<std::uint8_t>((y * 255) / height_);
                f.data[i + 2] = std::abs(x - band) < 10
                                    ? static_cast<std::uint8_t>(255)
                                    : static_cast<std::uint8_t>(0);
            }
        }
        return f;
    }

    int width_ = 640;
    int height_ = 480;
    int frame_index_ = 0;
    bool opened_ = false;
    std::chrono::steady_clock::time_point start_{};
};

} // namespace

std::unique_ptr<Camera> makeSyntheticCamera() {
    return std::make_unique<SyntheticCamera>();
}

} // namespace oc::detail
