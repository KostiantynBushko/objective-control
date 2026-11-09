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
  streaming, and live preview, plus a built-in `--help`.
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
does one of: grab frames, save a frame, record video, stream live, or open a
preview window.

```sh
./build/debug/bin/objective-control-app --help
```

`--help` (or `-h`) prints the complete option list with defaults and exits
with status `0`. It needs no camera and works on its own.

### Options

| Option                | Default   | Meaning                                               |
|-----------------------|-----------|-------------------------------------------------------|
| `--mode=<m>`          | `null`    | `null`, `embedded`, `ip`, or `synthetic`              |
| `--source=<s>`        | *(empty)* | device path/index (`embedded`) or URL (`ip`)          |
| `--width=N`           | `1920`    | requested frame width                                  |
| `--height=N`          | `1080`    | requested frame height                                 |
| `--fps=N`             | `30`      | requested frame rate                                   |
| `--frames=N`          | `1`       | number of frames to grab                               |
| `--seconds=N`         | `5`       | recording duration in seconds                          |
| `--out=<path>`        | *(none)*  | save the first captured frame as a PPM (P6) image     |
| `--record=<path>`     | *(none)*  | record video to `.mp4` or `.avi`                       |
| `--stream=<port>`     | *(off)*   | serve MJPEG over HTTP on `<port>` (runs until Ctrl+C) |
| `--preview`           | *(off)*   | live preview window (`q`/Esc to quit)                 |
| `--window-size=<s>`   | `native`  | preview window size: `native` or `<width>x<height>`   |
| `--window-fullscreen` | *(off)*   | make the preview window fullscreen                    |
| `--faces`            | *(off)*   | run face detection on each captured frame; drawn on the image in `--preview`/`--stream` |
| `--face-model=<path>`| *(default)*| YuNet model file used by `--faces`                   |
| `--face-score=<f>`   | *(default)*| minimum detection confidence to report a face       |
| `--face-detect-width=<px>` | *(default)* | width the detector runs at (0 = native)  |
| `--recognize`        | *(off)*   | identify each detected face against the enrolled set  |
| `--face-db=<path>`   | *(none)*  | face database file (load + save)                      |
| `--enroll=<id>`      | *(none)*  | enrol the best face of the first frame with one       |
| `--face-recognition-model=<path>` | *(default)* | SFace model file used by `--recognize`/`--enroll` |
| `--face-max-distance=<f>` | `0.4` | largest cosine distance still counted as a match      |
| `-h`, `--help`        | —         | print the option list and exit                         |

> Every option takes its value with `=` (`--width=640`, **not** `--width 640`).
> An unknown option, a stray argument, or an invalid `--mode` exits with
> status `2` and a one-line explanation.

### Choosing a backend (`--mode`)

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

### Capture frames (`--frames`, `--width`, `--height`, `--fps`)

```sh
# One frame at the requested resolution (the default)
./build/debug/bin/objective-control-app \
  --mode=synthetic --width=640 --height=480

# Grab 10 frames and print their size and timestamp
./build/debug/bin/objective-control-app \
  --mode=embedded --source=/dev/video0 --frames=10

# Ask the camera for 1080p30
./build/debug/bin/objective-control-app \
  --mode=embedded --source=/dev/video0 --width=1920 --height=1080 --fps=30
```

### Save a single frame (`--out`)

```sh
# Save the first captured frame as a binary PPM (P6) image
./build/debug/bin/objective-control-app \
  --mode=synthetic --width=640 --height=480 --out=frame.ppm
```

> PPM output needs a 3-channel (BGR) frame.

### Record video (`--record`, `--seconds`)

```sh
# Record a few seconds of video (OpenCV + FFmpeg)
./build/debug/bin/objective-control-app \
  --mode=synthetic --width=640 --height=480 --fps=30 \
  --seconds=5 --record=clip.mp4      # or clip.avi

# Record 30 seconds from a real device at 1080p30
./build/debug/bin/objective-control-app \
  --mode=embedded --source=/dev/video0 --width=1920 --height=1080 --fps=30 \
  --seconds=30 --record=clip.mp4
```

> The extension picks the codec: `.mp4` → MPEG-4 Part 2, `.avi` → MJPG.

### Stream live video (`--stream`)

```sh
# MJPEG-over-HTTP, viewable in a browser at http://localhost:8080/
# Several clients may watch at once; each receives the newest frame.
./build/debug/bin/objective-control-app \
  --mode=embedded --source=/dev/video0 --fps=15 --stream=8080

# Stream an IP camera to a browser
./build/debug/bin/objective-control-app \
  --mode=ip --source=rtsp://host:554/stream --fps=15 --stream=8080
```

> `http://localhost:<port>/` serves a small HTML page embedding the stream;
> `http://localhost:<port>/stream` is the raw MJPEG stream.
>
> **Security note:** the MJPEG stream server has no authentication and is
> intended for localhost or trusted networks only.

### Live preview (`--preview`, `--window-size`, `--window-fullscreen`)

```sh
# Live preview window (requires a display); press 'q' or Esc to quit
./build/debug/bin/objective-control-app \
  --mode=embedded --source=/dev/video0 --preview

# Preview in a specific window size
./build/debug/bin/objective-control-app \
  --mode=ip --source=rtsp://host:554/stream --preview --window-size=1280x720

# Fullscreen preview
./build/debug/bin/objective-control-app \
  --mode=ip --source=rtsp://host:554/stream --preview --window-fullscreen

# Preview an RTSP camera with the detected faces drawn on the frames
./build/debug/bin/objective-control-app \
  --mode=ip --source=rtsp://host:554/stream --preview --faces
```

> The preview window is opened at the camera's native resolution by default,
> so the picture is shown 1:1 rather than stretched. Use `--window-size` to
> pick a different size (`native`, or `<width>x<height>`).

> The preview and `--stream` always show the **newest** frame the camera has
> sent. A network backend decodes into a queue, and a consumer that is slower
> than the camera would otherwise be handed the oldest queued frame — the
> picture would then be several seconds behind. Frames that piled up are
> dropped instead. A recording (`--record`) keeps every frame and is not
> affected.

> Adding `--faces` (or `--recognize`) draws a box around every detected face.
> A recognised person gets a green box labelled with their id, an unknown
> face an amber box labelled `unknown`. The same overlay is drawn on the
> frames served by `--stream`, so the MJPEG page shows it too. Detection
> costs roughly 25 ms per frame, which caps the preview at a lower frame
> rate than the raw capture.

### Exit codes

| Code | Meaning                                                            |
|------|--------------------------------------------------------------------|
| `0`  | success                                                            |
| `1`  | runtime error (camera could not be opened, no frames captured, ...) |
| `2`  | bad usage (unknown option, invalid mode)                          |

## Backends

| Backend    | Source type          | Dependency              |
|------------|----------------------|-------------------------|
| `Null`     | —                    | built-in                 |
| `Synthetic`| —                    | built-in                 |
| `Embedded` | `/dev/video0`, `0`   | OpenCV (V4L2)            |
| `Ip`       | `rtsp://...`, `http://...` (MJPEG or snapshot) | RTSP: OpenCV/FFmpeg; HTTP: built-in client + `imgcodecs` |

### Face detection and recognition

Face detection and recognition are provided by the
[`face-detection`](third_party/face-detection) submodule and are **off by
default**. Enable them when configuring:

```sh
cmake --preset debug -DOBJECTIVE_CONTROL_ENABLE_FACE=ON
cmake --build --preset debug
```

The submodule's model weights are not committed; fetch them once:

```sh
third_party/face-detection/models/fetch.sh
```

Then run the app with `--faces`:

```sh
./build/debug/bin/objective-control-app \
    --mode=embedded --source=/dev/video0 --frames=10 --faces
```

```
Face detection enabled (model: third_party/face-detection/models/face_detection_yunet_2023mar.onnx).
Frame 0: 1920x1080 (3 ch), 6220800 bytes, t=6454us.
  faces: 2 (412,180 213x280 score=0.918774) (1244,201 226x296 score=0.909496)
Captured 1 frame(s).
Detected 2 face(s) in total.
```

Captured frames are handed to the detector without copying any pixels: `oc::Frame`
and `fd::Image` share the same dense BGR layout.

The same detector runs in `--preview` and `--stream` mode, where the boxes are
drawn onto the image instead of only being printed. The preview and the
MJPEG stream therefore show the faces even though the console stays quiet.

Detection is tunable, which matters for a fixed IP camera whose sub-stream is
small or whose subject is far away:

```sh
# Report faces down to 30% confidence, and run the detector on a 480px-wide
# copy of the frame (cheaper, and catches small faces better)
./build/debug/bin/objective-control-app \
    --mode=ip --source=rtsp://host:554/stream --preview --faces \
    --face-score=0.3 --face-detect-width=480
```

`--face-detect-width` scales the frame down before detection, so a 1080p
source costs about the same as a 640x480 one. Set it to `0` to detect at
native resolution.

Detection dominates the per-frame cost, and the per-frame cost decides how
smooth the preview is. Measured on a 1080p source, one frame of detection
costs about 8 ms at the default 960px width and about 2.5 ms at 480px.
Lowering the width is the cheapest way to buy back frame rate; a camera
sub-stream is even better, because it cuts the decode and the copies too.

#### Face recognition

`--recognize` turns the detected faces into identities. It needs the SFace
model (fetched by the same `fetch.sh` script) and a database of enrolled
people.

Enrol a person first — the most confident face of the first frame that
contains one is stored under the given id:

```sh
./build/debug/bin/objective-control-app \
    --mode=embedded --source=/dev/video0 --enroll=42 --face-db=faces.db
```

Then identify whoever is in front of the camera:

```sh
./build/debug/bin/objective-control-app \
    --mode=embedded --source=/dev/video0 --frames=10 \
    --recognize --face-db=faces.db
```

```
Face detection enabled (model: third_party/face-detection/models/face_detection_yunet_2023mar.onnx).
Face recognition enabled (model: third_party/face-detection/models/face_recognition_sface_2021dec.onnx).
Loaded 1 enrolment(s) from faces.db.
Frame 0: 1920x1080 (3 ch), 6220800 bytes, t=6454us.
  faces: 2 (412,180 213x280 score=0.918774) -> person 42 (distance 0.0832154) (1244,201 226x296 score=0.909496) -> unknown (distance 0.571002)
Captured 1 frame(s).
Detected 2 face(s) in total.
Recognised 1 face(s) against the database.
```

`--enroll` requires `--face-db`, because an enrolment that is never written
to disk is lost. The database stores **embeddings only, never photographs**,
and uses the same file format as the submodule's own app, so both can share
one `faces.db`.

`--face-max-distance` is the cosine-distance cut-off for a match. The default
of `0.4` is the usual "same person" threshold for SFace; lower it to be
stricter, raise it if legitimate matches are being rejected.

#### Locating the model files

The model weights are looked up **relative to the current working directory**,
not relative to the executable. The defaults therefore only resolve when the
app is launched from the repository root; from anywhere else you get:

```text
Could not load the detector model: OpenCV(4.10.0) ./modules/dnn/src/onnx/onnx_importer.cpp:277:
error: (-5:Bad argument) Can't read ONNX file: models/face_detection_yunet_2023mar.onnx
in function 'ONNXImporter'
```

| App | Option | Default |
|-----|--------|---------|
| `objective-control-app` | `--face-model=<file>` | `third_party/face-detection/models/face_detection_yunet_2023mar.onnx` (CWD-relative) |
| `objective-control-app` | `--face-recognition-model=<file>` | `third_party/face-detection/models/face_recognition_sface_2021dec.onnx` (CWD-relative) |
| `face-detection-app` | `--models=<dir>` | `$FACE_DETECTION_MODELS`, else `models` (CWD-relative) |

`face-detection-app` (built from the submodule) resolves the directory in this
order:

1. `--models=<dir>`
2. the `FACE_DETECTION_MODELS` environment variable
3. `./models`, relative to the working directory

`objective-control-app` takes a full file path via `--face-model` and does not
read `FACE_DETECTION_MODELS`.

Point either app at the weights explicitly:

```sh
# Parent app — the path is relative to the working directory
./build/debug/bin/objective-control-app --mode=synthetic --faces \
    --face-model=third_party/face-detection/models/face_detection_yunet_2023mar.onnx

# Submodule app
./third_party/face-detection/build/debug/bin/face-detection-app \
    --image=photo.jpg --models=third_party/face-detection/models
```

Or export the directory once for the current shell session — no need to edit
`~/.bashrc`:

```sh
export FACE_DETECTION_MODELS="$PWD/third_party/face-detection/models"
```

The simplest option is to launch the apps from the repository root, where the
relative defaults resolve on their own.

## CMake options

| Option                                  | Default | Purpose                             |
|-----------------------------------------|---------|-------------------------------------|
| `OBJECTIVE_CONTROL_ENABLE_OPENCV`       | `ON`    | Embedded/IP backends, record/stream |
| `OBJECTIVE_CONTROL_ENABLE_GSTREAMER`    | `OFF`   | GStreamer pipelines (future)        |
| `OBJECTIVE_CONTROL_BUILD_APPS`          | `ON`    | Build the example app               |
| `OBJECTIVE_CONTROL_BUILD_TESTS`         | `ON`    | Build + register tests              |
| `OBJECTIVE_CONTROL_ENABLE_SANITIZERS`   | `OFF`   | ASan/UBSan in debug builds          |
| `OBJECTIVE_CONTROL_ENABLE_FACE`         | `OFF`   | Use the face-detection submodule    |

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

## Related projects

| Project | Description |
|---------|-------------|
| [`third_party/face-detection`](third_party/face-detection) | Standalone face detection and recognition library (OpenCV YuNet + SFace, C++20). Consumed as a git submodule — see [Submodules](#submodules). |

`face-detection`'s `fd::Image` has exactly the same layout as `oc::Frame`
(dense, row-major BGR or grayscale), so a captured frame can be handed to the
face pipeline without copying a single pixel:

```cpp
fd::Image frame;
frame.width   = captured->width;
frame.height  = captured->height;
frame.channels = captured->channels;
frame.data    = captured->data;   // no copy
```

## Submodules

This repository uses a git submodule:

| Submodule | Path | Purpose |
|-----------|------|---------|
| `face-detection` | `third_party/face-detection` | Face detection and recognition library |

Clone with the submodules:

```sh
git clone --recurse-submodules <url>
```

For an existing clone:

```sh
git submodule update --init --recursive
```

The submodule's model weights are not committed; fetch them once with
`third_party/face-detection/models/fetch.sh`.
