# Object library: objective_control

add_library(objective_control
    camera.cpp
    backends/null_camera.cpp
    backends/synthetic_camera.cpp
)

add_library(objective-control::objective_control ALIAS objective_control)

target_include_directories(objective_control
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# OpenCV-backed backends (embedded + IP).
if(OC_HAVE_OPENCV)
    target_compile_definitions(objective_control PUBLIC OC_HAVE_OPENCV=1)
    target_sources(objective_control PRIVATE
        backends/embedded_camera.cpp
    )
    target_link_libraries(objective_control PUBLIC ${OC_OPENCV_TARGETS})
endif()

# FFmpeg-backed RTSP backend: low-latency decoding of a network stream.
# Without it the RTSP path falls back to OpenCV's VideoCapture, which
# buffers frames and shows a live preview seconds behind the camera.
if(OC_HAVE_FFMPEG)
    target_compile_definitions(objective_control PUBLIC OC_HAVE_FFMPEG=1)
    target_sources(objective_control PRIVATE
        backends/rtsp_ffmpeg.cpp
    )
    target_link_libraries(objective_control PUBLIC ${OC_FFMPEG_TARGETS})
endif()

# The IP backend dispatches RTSP and HTTP; it needs at least one of the two.
if(OC_HAVE_OPENCV OR OC_HAVE_FFMPEG)
    target_sources(objective_control PRIVATE backends/ip_camera.cpp)
endif()

# The tracker of the faces that are not in the database yet. It needs the
# embeddings of the face-detection submodule, so it is only built when that
# submodule is used.
if(OBJECTIVE_CONTROL_HAVE_FACE)
    target_sources(objective_control PRIVATE
        face/auto_enroller.cpp
    )
endif()

# GStreamer backend (future).
if(OC_HAVE_GSTREAMER)
    target_compile_definitions(objective_control PUBLIC OC_HAVE_GSTREAMER=1)
    target_link_libraries(objective_control PUBLIC ${OC_GSTREAMER_TARGETS})
endif()

warn_on(objective_control)
