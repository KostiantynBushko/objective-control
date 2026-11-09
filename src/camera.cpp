#include "objective-control/camera/camera.hpp"
#include "backends/backends_internal.hpp"

namespace oc {

std::unique_ptr<Camera> Camera::create(CameraKind kind) {
    switch (kind) {
        case CameraKind::Null:
            return detail::makeNullCamera();
        case CameraKind::Embedded:
#ifdef OC_HAVE_OPENCV
            return detail::makeEmbeddedCamera();
#else
            return nullptr;
#endif
        case CameraKind::Ip:
#ifdef OC_HAVE_OPENCV
            return detail::makeIpCamera();
#else
            return nullptr;
#endif
        case CameraKind::Synthetic:
            return detail::makeSyntheticCamera();
    }
    return nullptr;
}

} // namespace oc
