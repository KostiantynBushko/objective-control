#pragma once

// Internal factory for the FFmpeg-backed RTSP camera. Private to the
// library; not part of the public API.

#include "objective-control/camera/camera.hpp"

#include <memory>

namespace oc::detail {

// Creates the low-latency RTSP backend. Returns nullptr when the library
// was built without FFmpeg.
std::unique_ptr<Camera> makeRtspCamera();

} // namespace oc::detail
