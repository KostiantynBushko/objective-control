# Example application: demonstrates the camera interface + backend factory.

add_executable(objective-control-app
    camera_demo.cpp
)

target_link_libraries(objective-control-app PRIVATE objective_control Threads::Threads)
warn_on(objective-control-app)
