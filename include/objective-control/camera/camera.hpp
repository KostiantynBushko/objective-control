#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oc {

/// A single decoded image frame, in dense (row-major) BGR or grayscale.
struct Frame {
    int width = 0;
    int height = 0;
    int channels = 0;          ///< 1 (gray) or 3 (BGR).
    std::int64_t timestamp_us = 0; ///< Monotonic capture time, microseconds.
    std::vector<std::uint8_t> data; ///< Pixel data, width*height*channels bytes.
};

/// Backend category. The factory maps each to the appropriate implementation.
enum class CameraKind : std::uint8_t {
    Null,      ///< No-op placeholder (always available).
    Embedded,  ///< Local USB/CSI/MIPI device (e.g. /dev/video0 or index).
    Ip,        ///< Network camera: RTSP, HTTP MJPEG, or HTTP snapshot.
    Synthetic, ///< In-memory test pattern (no hardware; always available).
};

/// Runtime configuration for opening a camera.
struct CameraConfig {
    CameraKind kind = CameraKind::Null;

    /// Embedded: device path ("/dev/video0") or numeric index ("0").
    /// Ip:       RTSP URL ("rtsp://host:554/stream"), an MJPEG-over-HTTP
    ///           stream ("http://host:8080/?action=stream"), or a snapshot
    ///           URL ("http://host/axis-cgi/jpg/image.cgi").
    std::string source;

    int width = 1920;
    int height = 1080;
    int fps = 30;
};

/// Abstract camera interface implemented by each backend.
class Camera {
public:
    virtual ~Camera() = default;

    /// Open the device described by @p config. Returns false on failure.
    virtual bool open(const CameraConfig& config) = 0;

    /// Release the device. Safe to call multiple times / when never opened.
    virtual void close() = 0;

    /// True while a device is opened and ready.
    virtual bool isOpen() const = 0;

    /// Capture one frame. Returns std::nullopt if no frame is available yet
    /// (timeout, dropped frame, or device error).
    virtual std::optional<Frame> grab() = 0;

    /// Factory: construct the backend for the requested kind. Returns nullptr
    /// if the kind has no implementation compiled in.
    static std::unique_ptr<Camera> create(CameraKind kind);
};

} // namespace oc
