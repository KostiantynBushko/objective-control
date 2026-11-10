#include "objective-control/camera/camera.hpp"
#include "objective-control/version.hpp"

#include <chrono>
#include <iostream>
#include <thread>

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

    // Live mode asks a backend for the newest frame instead of the oldest
    // buffered one. Backends that do not buffer must be unaffected by it.
    auto synth = Camera::create(CameraKind::Synthetic);
    check(static_cast<bool>(synth), "synthetic backend is created");

    CameraConfig liveCfg;
    liveCfg.kind = CameraKind::Synthetic;
    liveCfg.width = 64;
    liveCfg.height = 48;
    liveCfg.live = true;

    check(synth->open(liveCfg), "synthetic backend opens in live mode");
    const auto first = synth->grab();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto second = synth->grab();
    check(first.has_value() && second.has_value(),
          "synthetic backend returns frames in live mode");
    if (first && second) {
        check(first->width == 64 && first->height == 48,
              "synthetic frame honours the requested size");
        // The timestamp is what tells a consumer how old a frame is, so it
        // has to advance with the clock.
        check(second->timestamp_us > first->timestamp_us,
              "synthetic frame timestamp advances with the clock");
    }
    synth->close();

    // The IP backend is available when the library was built with OpenCV or
    // FFmpeg. It has to reject a source it cannot open instead of throwing
    // or hanging.
#if defined(OC_HAVE_OPENCV) || defined(OC_HAVE_FFMPEG)
    auto ip = Camera::create(CameraKind::Ip);
    check(static_cast<bool>(ip), "ip backend is created");

    CameraConfig ipCfg;
    ipCfg.kind = CameraKind::Ip;
    ipCfg.source = "";
    check(!ip->open(ipCfg), "ip backend rejects an empty source");

    // A malformed RTSP URL must fail as well, and it must do so quickly.
    ipCfg.source = "rtsp://";
    ipCfg.transport = "udp";
    check(!ip->open(ipCfg), "ip backend rejects a malformed RTSP URL");
    ip->close();
    check(!ip->isOpen(), "ip backend reports closed after close");
#endif

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
