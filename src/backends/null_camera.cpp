#include "backends/backends_internal.hpp"

namespace oc::detail {

namespace {

class NullCamera final : public Camera {
public:
    bool open(const CameraConfig&) override { opened_ = true; return true; }
    void close() override { opened_ = false; }
    bool isOpen() const override { return opened_; }
    std::optional<Frame> grab() override { return std::nullopt; }

private:
    bool opened_ = false;
};

} // namespace

std::unique_ptr<Camera> makeNullCamera() {
    return std::make_unique<NullCamera>();
}

} // namespace oc::detail
