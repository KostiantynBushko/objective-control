#include "objective-control/camera/camera.hpp"
#include "objective-control/version.hpp"

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    using oc::Camera;
    using oc::CameraConfig;
    using oc::CameraKind;

    // Null backend is always available and must round-trip the state machine.
    auto cam = Camera::create(CameraKind::Null);
    check(static_cast<bool>(cam), "null backend is created");

    CameraConfig cfg;
    cfg.kind = CameraKind::Null;

    check(cam->open(cfg), "null backend opens");
    check(cam->isOpen(), "null backend reports open");
    check(!cam->grab().has_value(), "null backend returns no frame");

    cam->close();
    check(!cam->isOpen(), "null backend reports closed after close");

    // Version header is populated by CMake at configure time.
    const std::string version = OC_VERSION_STRING;
    check(!version.empty() && version.front() >= '0' && version.front() <= '9',
          "version string is populated");

    if (failures == 0) {
        std::cout << "camera_test: all checks passed.\n";
        return 0;
    }
    std::cerr << "camera_test: " << failures << " check(s) failed.\n";
    return 1;
}
