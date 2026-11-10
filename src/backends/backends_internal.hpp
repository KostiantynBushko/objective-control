#pragma once

// Internal backend factory declarations. These are private to the library and
// are not installed as part of the public API.

#include "objective-control/camera/camera.hpp"

namespace oc::detail {

std::unique_ptr<Camera> makeNullCamera();
std::unique_ptr<Camera> makeSyntheticCamera();

#ifdef OC_HAVE_OPENCV
std::unique_ptr<Camera> makeEmbeddedCamera();
#endif

#if defined(OC_HAVE_OPENCV) || defined(OC_HAVE_FFMPEG)
std::unique_ptr<Camera> makeIpCamera();
#endif

} // namespace oc::detail
