# Dependencies.cmake
#
# Central place for discovering optional third-party dependencies.
# Each backend exposes:
#   * a boolean <X>_FOUND / OC_HAVE_<X> switch for compile-time #ifdefs
#   * an imported CMake target (or list of targets) used to link
#
# This keeps the CMakeLists.txt files clean and makes it easy to stub out a
# dependency on machines where it is not installed.

# ---------------------------------------------------------------------------
# OpenCV — used by the "embedded" (USB/CSI) and "IP" (RTSP/HTTP) backends.
# Try the canonical config package first, then fall back to pkg-config.
# ---------------------------------------------------------------------------
set(OC_HAVE_OPENCV FALSE)
set(OC_OPENCV_TARGETS "")

if(OBJECTIVE_CONTROL_ENABLE_OPENCV)
    find_package(OpenCV QUIET COMPONENTS core videoio imgproc imgcodecs highgui)

    if(OpenCV_FOUND)
        set(OC_HAVE_OPENCV TRUE)
        set(OC_OPENCV_TARGETS ${OpenCV_LIBS})
        message(STATUS "OpenCV ${OpenCV_VERSION} found (config package)")
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(OpenCV IMPORTED_TARGET QUIET opencv4)
            if(OpenCV_FOUND)
                set(OC_HAVE_OPENCV TRUE)
                set(OC_OPENCV_TARGETS PkgConfig::OpenCV)
                message(STATUS "OpenCV found via pkg-config (opencv4)")
            endif()
        endif()
    endif()

    if(NOT OC_HAVE_OPENCV)
        message(WARNING
            "OpenCV not found. Building WITHOUT embedded/IP camera backends "
            "(null backend only). Install e.g. 'sudo apt install libopencv-dev' "
            "or point OpenCV_DIR/pkg-config at an OpenCV installation.")
    endif()
endif()

# ---------------------------------------------------------------------------
# FFmpeg — used by the RTSP backend to decode a network stream with the
# low-latency settings a live preview needs (no packet buffering, no frame
# reordering delay). Without it the RTSP path falls back to OpenCV's
# VideoCapture, which buffers frames and shows the picture seconds late.
# ---------------------------------------------------------------------------
set(OC_HAVE_FFMPEG FALSE)
set(OC_FFMPEG_TARGETS "")

if(OBJECTIVE_CONTROL_ENABLE_FFMPEG)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFmpeg IMPORTED_TARGET QUIET
            libavformat libavcodec libavutil libswscale)
        if(FFmpeg_FOUND)
            set(OC_HAVE_FFMPEG TRUE)
            set(OC_FFMPEG_TARGETS PkgConfig::FFmpeg)
            execute_process(
                COMMAND ${PKG_CONFIG_EXECUTABLE} --modversion libavformat
                OUTPUT_VARIABLE FFMPEG_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            message(STATUS "FFmpeg ${FFMPEG_VERSION} found (pkg-config)")
        endif()
    endif()

    if(NOT OC_HAVE_FFMPEG)
        message(WARNING
            "FFmpeg not found. The RTSP backend falls back to OpenCV's "
            "VideoCapture, which shows a live preview with a delay of "
            "seconds. Install e.g. 'sudo apt install libavformat-dev "
            "libavcodec-dev libavutil-dev libswscale-dev' for the "
            "low-latency backend.")
    endif()
endif()

# ---------------------------------------------------------------------------
# GStreamer — optional alternative backend for advanced pipelines.
# ---------------------------------------------------------------------------
set(OC_HAVE_GSTREAMER FALSE)
set(OC_GSTREAMER_TARGETS "")

if(OBJECTIVE_CONTROL_ENABLE_GSTREAMER)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(Gstreamer IMPORTED_TARGET QUIET gstreamer-1.0 gstreamer-app-1.0)
        if(Gstreamer_FOUND)
            set(OC_HAVE_GSTREAMER TRUE)
            set(OC_GSTREAMER_TARGETS PkgConfig::Gstreamer)
            message(STATUS "GStreamer found via pkg-config")
        endif()
    endif()

    if(NOT OC_HAVE_GSTREAMER)
        message(WARNING "GStreamer not found; continuing without GStreamer support.")
    endif()
endif()
