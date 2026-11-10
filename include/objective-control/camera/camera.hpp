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

    /// RTSP transport: "udp" (default), "tcp" or empty for the default.
    ///
    /// UDP gives the lowest latency -- a lost TCP segment holds the whole
    /// stream back until it is retransmitted. TCP is more reliable on a
    /// lossy network, and some cameras do not accept UDP at all, so the
    /// backend falls back to TCP when the UDP session cannot be opened.
    std::string transport;

    /// Live mode: hand out the newest frame instead of the oldest one the
    /// backend has buffered.
    ///
    /// A network backend keeps decoding into a queue. When the consumer is
    /// slower than the camera, that queue only grows and every frame handed
    /// out is older than the last -- the preview then shows the scene as it
    /// was several seconds ago. Live mode discards the frames that piled up
    /// so the consumer always sees the current picture.
    ///
    /// Enable it for previews and live streaming. Leave it off when every
    /// frame has to be kept, such as a recording.
    bool live = false;
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
