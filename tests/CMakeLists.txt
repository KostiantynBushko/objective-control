# Minimal, dependency-free smoke test (no GTest required).

add_executable(camera_test
    camera_test.cpp
)

target_link_libraries(camera_test PRIVATE objective_control)
warn_on(camera_test)

add_test(NAME camera_test COMMAND camera_test)

# The tracker of the faces that are not in the database yet. It needs the
# face-detection submodule, so it is only tested when that submodule is
# used.
if(OBJECTIVE_CONTROL_HAVE_FACE)
    add_executable(auto_enroller_test
        auto_enroller_test.cpp
    )

    target_link_libraries(auto_enroller_test PRIVATE objective_control)
    warn_on(auto_enroller_test)

    add_test(NAME auto_enroller_test COMMAND auto_enroller_test)
endif()
