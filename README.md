# objective-control

A cross-platform **C++20 library** for connecting to cameras — both **embedded**
devices (USB / CSI / MIPI) and **IP cameras** (RTSP / HTTP). It exposes a single
`Camera` abstraction so callers don't care which kind of device is underneath.

```
Capture (any backend) ──► Frame ──► save (PPM), record (MP4/AVI),
                                   stream (MJPEG over HTTP), or preview
```

## Features

- Uniform `Camera` interface: `open` / `close` / `isOpen` / `grab` + a factory.
- Backends: `Null` (built-in), `Synthetic` (built-in test pattern), `Embedded`
  (OpenCV V4L2), `Ip` (network camera — RTSP / HTTP MJPEG / HTTP snapshot).
- Example CLI app with modes for frame capture, video recording, MJPEG
  streaming, and live preview.
- Optional dependencies degrade gracefully (e.g. no OpenCV → still builds with
  the built-in backends).
- CMake presets (debug / release / asan) and strict compiler warnings.

## Requirements

- CMake ≥ 3.20, a C++20 compiler (GCC 11+/Clang 14+/MSVC).
- [OpenCV](https://opencv.org/) ≥ 4.x (recommended; enables `Embedded`/`Ip`
  backends, video recording, MJPEG streaming, and preview). On Debian/Ubuntu:
  ```sh
  sudo apt install libopencv-dev
  ```
- Optional: FFmpeg (for MP4/AVI recording), a display (for `--preview`).

## Build

> **Run every command below from the repository root** — the directory that
> contains `CMakeLists.txt`, `CMakePresets.json`, `src/`, `apps/` and `tests/`.
>
> ```text
> objective-control/          <-- run cmake / ctest from HERE
> |-- CMakeLists.txt
> |-- CMakePresets.json
> |-- src/
> |-- apps/
> |-- tests/
> ```
>
> CMake and CTest read `CMakePresets.json` from the *current working directory
> only* and do not search parent directories. Running the commands from a
> subdirectory such as `src/` therefore fails with:
>
> ```text
> CMake Error: Could not read presets from /path/to/objective-control/src:
> File not found: /path/to/objective-control/src/CMakePresets.json
> ```

### Using presets

```sh
cmake --preset debug        # or: release, asan
cmake --build --preset debug
ctest --preset default
```

### Without presets

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

> This form must also be run from the repository root: `-S .` and `-B build`
> are resolved relative to the current working directory.

Binaries are written to `build/<preset>/bin/` (e.g.
`build/debug/bin/objective-control-app`).

## Library API

Everything lives in namespace `oc` (see
[`include/objective-control/camera/camera.hpp`](include/objective-control/camera/camera.hpp)).

```cpp
#include "objective-control/camera/camera.hpp"

oc::CameraConfig cfg;
cfg.kind   = oc::CameraKind::Embedded;
cfg.source = "/dev/video0";      // path, or numeric index like "0"
cfg.width  = 1920;
cfg.height = 1080;
cfg.fps    = 30;

auto camera = oc::Camera::create(cfg.kind);
if (!camera || !camera->open(cfg)) {
    // handle failure
}

if (auto frame = camera->grab()) {
    // frame->width, frame->height, frame->channels (1 = gray, 3 = BGR),
    // frame->timestamp_us, frame->data (width*height*channels bytes)
}

camera->close();
```

Key types:

| Type           | Members                                                                 |
|----------------|-------------------------------------------------------------------------|
| `CameraKind`   | `Null`, `Embedded`, `Ip`, `Synthetic`                                   |
| `CameraConfig` | `kind`, `source`, `width` (1920), `height` (1080), `fps` (30)            |
| `Frame`        | `width`, `height`, `channels`, `timestamp_us`, `data` (BGR or gray)      |
| `Camera`       | `open`, `close`, `isOpen`, `grab` (`std::optional<Frame>`), `create`     |

## Command-line app

The example app (`objective-control-app`) selects a backend with `--mode` and
does one of: grab frames, save a frame, record video, or stream live.

### Modes

```sh
# Embedded: USB/CSI device (path or numeric index)
./build/debug/bin/objective-control-app --mode=embedded --source=/dev/video0

# IP camera over RTSP
./build/debug/bin/objective-control-app --mode=ip --source=rtsp://user:pass@host:554/stream

# Network webcam over HTTP MJPEG (mjpg-streamer, Motion, IP cam MJPEG endpoint)
./build/debug/bin/objective-control-app --mode=ip --source=http://host:8080/?action=stream

# Network webcam with a single-JPEG snapshot URL (polled)
./build/debug/bin/objective-control-app --mode=ip --source=http://host/axis-cgi/jpg/image.cgi

# Built-in moving test pattern (no hardware needed)
./build/debug/bin/objective-control-app --mode=synthetic --width=640 --height=480

# No-op placeholder
./build/debug/bin/objective-control-app --mode=null
```

### Capture a single frame (saved as PPM)

```sh
./build/debug/bin/objective-control-app \
  --mode=synthetic --width=640 --height=480 --out=frame.ppm
```

### Record a few seconds of video (OpenCV + FFmpeg)

```sh
./build/debug/bin/objective-control-app \
  --mode=synthetic --width=640 --height=480 --fps=30 \
  --seconds=5 --record=clip.mp4      # or clip.avi
```

### Stream live video (no saving)

```sh
# MJPEG-over-HTTP, viewable in a browser at http://localhost:8080/
# Several clients may watch at once; each receives the newest frame.
./build/debug/bin/objective-control-app \
  --mode=embedded --source=/dev/video0 --fps=15 --stream=8080

# Live preview window (requires a display)
./build/debug/bin/objective-control-app \
  --mode=embedded --source=/dev/video0 --preview

# Preview in a specific window size, or fullscreen
./build/debug/bin/objective-control-app \
  --mode=ip --source=rtsp://host:554/stream --preview --window-size=1280x720
./build/debug/bin/objective-control-app \
  --mode=ip --source=rtsp://host:554/stream --preview --window-fullscreen
```

> The preview window is opened at the camera's native resolution by default,
> so the picture is shown 1:1 rather than stretched. Use `--window-size` to
> pick a different size (`native`, or `<width>x<height>`).

### Flags

| Flag              | Meaning                                              |
|-------------------|------------------------------------------------------|
| `--mode=<m>`      | `null`, `embedded`, `ip`, or `synthetic`             |
| `--source=<s>`    | device path/index (embedded) or URL (ip)             |
| `--width=N`       | requested frame width (default 1920)                 |
| `--height=N`      | requested frame height (default 1080)                |
| `--fps=N`         | requested frame rate (default 30)                    |
| `--frames=N`      | number of frames to grab (default 1)                 |
| `--seconds=N`     | recording duration in seconds (default 5)            |
| `--out=<path>`    | save the first frame as PPM                          |
| `--record=<path>` | record video to `.mp4` or `.avi`                     |
| `--stream=<port>` | serve MJPEG over HTTP on `<port>` (runs until Ctrl+C)|
| `--preview`       | live preview window (`q`/Esc to quit)                |
| `--window-size`   | preview window size: `native` (default) or `W x H`   |
| `--window-fullscreen` | make the preview window fullscreen             |

> **Security note:** the MJPEG stream server has no authentication and is
> intended for localhost or trusted networks only.

## Backends

| Backend    | Source type          | Dependency              |
|------------|----------------------|-------------------------|
| `Null`     | —                    | built-in                 |
| `Synthetic`| —                    | built-in                 |
| `Embedded` | `/dev/video0`, `0`   | OpenCV (V4L2)            |
| `Ip`       | `rtsp://...`, `http://...` (MJPEG or snapshot) | RTSP: OpenCV/FFmpeg; HTTP: built-in client + `imgcodecs` |

## CMake options

| Option                                  | Default | Purpose                             |
|-----------------------------------------|---------|-------------------------------------|
| `OBJECTIVE_CONTROL_ENABLE_OPENCV`       | `ON`    | Embedded/IP backends, record/stream |
| `OBJECTIVE_CONTROL_ENABLE_GSTREAMER`    | `OFF`   | GStreamer pipelines (future)        |
| `OBJECTIVE_CONTROL_BUILD_APPS`          | `ON`    | Build the example app               |
| `OBJECTIVE_CONTROL_BUILD_TESTS`         | `ON`    | Build + register tests              |
| `OBJECTIVE_CONTROL_ENABLE_SANITIZERS`   | `OFF`   | ASan/UBSan in debug builds          |

## Layout

```
include/objective-control/camera/camera.hpp   Public API (Camera, Frame, ...)
src/                                          Library implementation
src/backends/                                 Per-backend implementations
src/backends/http_mjpeg.hpp                   Self-contained HTTP MJPEG/snapshot client
apps/camera_demo.cpp                          Example CLI application
apps/mjpeg_stream.hpp                         Minimal MJPEG-over-HTTP server
tests/camera_test.cpp                         Smoke test (no external test deps)
cmake/                                        Dependency + warning helpers
CMakePresets.json                             Debug / release / asan presets
```
