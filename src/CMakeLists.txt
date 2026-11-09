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
        backends/ip_camera.cpp
    )
    target_link_libraries(objective_control PUBLIC ${OC_OPENCV_TARGETS})
endif()

# GStreamer backend (future).
if(OC_HAVE_GSTREAMER)
    target_compile_definitions(objective_control PUBLIC OC_HAVE_GSTREAMER=1)
    target_link_libraries(objective_control PUBLIC ${OC_GSTREAMER_TARGETS})
endif()

warn_on(objective_control)
