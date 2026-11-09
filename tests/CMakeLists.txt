# Minimal, dependency-free smoke test (no GTest required).

add_executable(camera_test
    camera_test.cpp
)

target_link_libraries(camera_test PRIVATE objective_control)
warn_on(camera_test)

add_test(NAME camera_test COMMAND camera_test)
