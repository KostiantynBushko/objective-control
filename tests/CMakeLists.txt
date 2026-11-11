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

    # The integration test of the deferred auto-enrolment: the tracker and
    # the store together, with a real database file.
    add_executable(auto_enroll_integration_test
        auto_enroll_integration_test.cpp
    )

    target_link_libraries(auto_enroll_integration_test PRIVATE objective_control)
    warn_on(auto_enroll_integration_test)

    add_test(NAME auto_enroll_integration_test
             COMMAND auto_enroll_integration_test)

    # The application itself: a run with --auto-enroll against a synthetic
    # camera and a file store. Nothing is written to the database, because
    # the faces of the run cannot be confirmed.
    #
    # The test needs the models of the face-detection submodule, and those
    # are downloaded rather than committed, so the test is only registered
    # when they are there.
    if(OBJECTIVE_CONTROL_BUILD_APPS)
        set(OC_FACE_MODELS_DIR "${CMAKE_SOURCE_DIR}/third_party/face-detection/models")
        if(EXISTS "${OC_FACE_MODELS_DIR}/face_detection_yunet_2023mar.onnx"
           AND EXISTS "${OC_FACE_MODELS_DIR}/face_recognition_sface_2021dec.onnx")
            add_test(NAME auto_enroll_cli_test
                     COMMAND ${CMAKE_COMMAND}
                             -DAPP=$<TARGET_FILE:objective-control-app>
                             -DDB=${CMAKE_CURRENT_BINARY_DIR}/auto_enroll_cli_test.db
                             -DDETECTOR=${OC_FACE_MODELS_DIR}/face_detection_yunet_2023mar.onnx
                             -DRECOGNIZER=${OC_FACE_MODELS_DIR}/face_recognition_sface_2021dec.onnx
                             -P ${CMAKE_CURRENT_SOURCE_DIR}/auto_enroll_cli_test.cmake)
        else()
            message(STATUS
                "The models of the face-detection submodule are missing, so "
                "the test of --auto-enroll is not registered. Run "
                "third_party/face-detection/models/fetch.sh to download "
                "them.")
        endif()
    endif()
endif()
