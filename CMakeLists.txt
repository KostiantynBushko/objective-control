cmake_minimum_required(VERSION 3.20)

project(objective-control
    VERSION 0.1.0
    DESCRIPTION "Cross-platform camera control library for embedded and IP cameras"
    HOMEPAGE_URL "https://example.local/objective-control"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Build-time options (turn features on/off without editing this file).
# ---------------------------------------------------------------------------
option(OBJECTIVE_CONTROL_ENABLE_OPENCV     "Enable OpenCV-backed embedded/IP camera backends" ON)
option(OBJECTIVE_CONTROL_ENABLE_GSTREAMER  "Enable GStreamer-based camera pipelines"          OFF)
option(OBJECTIVE_CONTROL_BUILD_APPS        "Build example applications"                        ${PROJECT_IS_TOP_LEVEL})
option(OBJECTIVE_CONTROL_BUILD_TESTS       "Build unit tests"                                  ${PROJECT_IS_TOP_LEVEL})
option(OBJECTIVE_CONTROL_ENABLE_SANITIZERS "Build with Address/Undefined sanitizers (debug)"   OFF)
option(OBJECTIVE_CONTROL_ENABLE_FACE       "Use the face-detection submodule"                    OFF)

# ---------------------------------------------------------------------------
# Load dependency discovery + compiler-warning helpers.
# ---------------------------------------------------------------------------
include(cmake/Dependencies.cmake)
include(cmake/Warnings.cmake)

# ---------------------------------------------------------------------------
# Global C++ settings.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Threading — used by the example app's MJPEG streaming server.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Optional sanitizers (debug builds only).
if(OBJECTIVE_CONTROL_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers are only supported on GCC/Clang.")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Generate a version header from the project() VERSION fields.
# ---------------------------------------------------------------------------
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/objective-control/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/objective-control/version.hpp
    @ONLY
)

# ---------------------------------------------------------------------------
# Face detection (optional submodule).
#
# The face-detection library lives in its own git repository and is consumed
# as a submodule at third_party/face-detection. It is OFF by default so that
# a clone made without "--recurse-submodules" still configures and builds.
# ---------------------------------------------------------------------------
set(OBJECTIVE_CONTROL_HAVE_FACE FALSE)

if(OBJECTIVE_CONTROL_ENABLE_FACE)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/face-detection/CMakeLists.txt)
        add_subdirectory(third_party/face-detection)
        set(OBJECTIVE_CONTROL_HAVE_FACE TRUE)
    else()
        message(WARNING
            "OBJECTIVE_CONTROL_ENABLE_FACE is ON but the face-detection "
            "submodule is not checked out. Run:\n"
            "  git submodule update --init --recursive")
    endif()
endif()

# ---------------------------------------------------------------------------
# Subdirectories.
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(OBJECTIVE_CONTROL_HAVE_FACE)
    target_link_libraries(objective_control PUBLIC face-detection::face_detection)
    target_compile_definitions(objective_control PUBLIC OC_HAVE_FACE=1)
endif()

if(OBJECTIVE_CONTROL_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(OBJECTIVE_CONTROL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
