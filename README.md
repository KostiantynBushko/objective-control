# objective-control

A cross-platform **C++20 library** for connecting to cameras — both **embedded**
devices (USB / CSI / MIPI) and **IP cameras** (RTSP / HTTP). It exposes a single
`Camera` abstraction so callers don't care which kind of device is underneath.

```
Capture (any backend) ──► Frame ──► save (PPM), record (MP4/AVI),
                                   stream (MJPEG over HTTP), or preview
```

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Build](#build)
  - [Using presets](#using-presets)
  - [Without presets](#without-presets)
- [Library API](#library-api)
- [Command-line app](#command-line-app)
  - [Options](#options)
  - [Choosing a backend (`--mode`)](#choosing-a-backend---mode)
  - [Capture frames (`--frames`, `--width`, `--height`, `--fps`)](#capture-frames---frames---width---height---fps)
  - [Save a single frame (`--out`)](#save-a-single-frame---out)
  - [Record video (`--record`, `--seconds`)](#record-video---record---seconds)
  - [Stream live video (`--stream`)](#stream-live-video---stream)
  - [Live preview (`--preview`, `--window-size`, `--window-fullscreen`)](#live-preview---preview---window-size---window-fullscreen)
  - [Exit codes](#exit-codes)
- [Backends](#backends)
  - [Face detection and recognition](#face-detection-and-recognition)
    - [Face recognition](#face-recognition)
    - [Automatic enrolment (`--auto-enroll`)](#automatic-enrolment---auto-enroll)
      - [The knobs](#the-knobs)
      - [What is stored](#what-is-stored)
      - [What the operator sees](#what-the-operator-sees)
      - [The old behaviour (`--auto-enroll-mode=immediate`)](#the-old-behaviour---auto-enroll-modeimmediate)
      - [Enriching the people that are enrolled already (`--auto-enroll-attach`)](#enriching-the-people-that-are-enrolled-already---auto-enroll-attach)
    - [Finding the people that were enrolled twice](#finding-the-people-that-were-enrolled-twice)
  - [Where the enrolments are stored](#where-the-enrolments-are-stored)
    - [PostgreSQL](#postgresql)
    - [The size of the face image (`--face-image-size`)](#the-size-of-the-face-image---face-image-size)
    - [Reviewing the enrolments](#reviewing-the-enrolments)
    - [Locating the model files](#locating-the-model-files)
- [CMake options](#cmake-options)
- [Layout](#layout)
- [Related projects](#related-projects)
- [Design notes](#design-notes)
- [Submodules](#submodules)

## Features

- Uniform `Camera` interface: `open` / `close` / `isOpen` / `grab` + a factory.
- Backends: `Null` (built-in), `Synthetic` (built-in test pattern), `Embedded`
  (OpenCV V4L2), `Ip` (network camera — RTSP / HTTP MJPEG / HTTP snapshot).
- Example CLI app with modes for frame capture, video recording, MJPEG
  streaming, and live preview, plus a built-in `--help`.
- Optional dependencies degrade gracefully (e.g. no OpenCV → still builds with
  the built-in backends).
- A desktop editor for the face database (`tools/face-db-editor`): review the
  enrolled people, correct their names and metadata, look at the picture of a
  face, and merge the people that are the same person.
- Optional face detection and recognition (OpenCV YuNet + SFace): enrol the
  people by hand, or let the camera enrol whoever shows up -- the unknown
  faces are watched for a while first, and a person is stored with several
  views of the face instead of one.
- CMake presets (debug / release / asan) and strict compiler warnings.

## Requirements

- CMake ≥ 3.20, a C++20 compiler (GCC 11+/Clang 14+/MSVC).
- [OpenCV](https://opencv.org/) ≥ 4.x (recommended; enables `Embedded`/`Ip`
  backends, video recording, MJPEG streaming, and preview). On Debian/Ubuntu:
  ```sh
  sudo apt install libopencv-dev
  ```
- [FFmpeg](https://ffmpeg.org/) development libraries (recommended; enables
  the low-latency RTSP backend). On Debian/Ubuntu:
  ```sh
  sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
  ```
  Without them the RTSP path falls back to OpenCV's `VideoCapture`, which
  buffers frames and shows a live preview about 1.7 s behind the camera.
- Optional: [PostgreSQL](https://www.postgresql.org/) client library
  (libpq), for storing the enrolled faces in a database.
  ```sh
  sudo apt install libpq-dev
  ```
  Without it the enrolments still work, but they are kept in a file.
- Optional: a display (for `--preview`).

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
| `CameraConfig` | `kind`, `source`, `width` (1920), `height` (1080), `fps` (30), `transport`, `live` |
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
| `--transport=<proto>` | `udp`     | RTSP transport: `udp` (lowest latency, falls back to `tcp`) or `tcp` |
| `--faces`            | *(off)*   | run face detection on each captured frame; drawn on the image in `--preview`/`--stream` |
| `--face-model=<path>`| `third_party/face-detection/models/face_detection_yunet_2023mar.onnx` | YuNet model file used by `--faces` |
| `--face-score=<f>`   | `0.6`    | minimum detection confidence to report a face       |
| `--face-detect-width=<px>` | `960` | width the detector runs at (0 = native)  |
| `--face-image-size=<px>` | `320` | size of the face image that is stored and recognised |
| `--recognize`        | *(off)*   | identify each detected face against the enrolled set  |
| `--face-db=<target>` | *(none)*  | face database: a file path or a `postgres://…` connection string (load + save) |
| `--face-db-list`     | *(off)*   | list the enrolled people and exit (no camera)         |
| `--face-db-remove=<id>` | *(none)* | remove every sample of a person and exit (no camera)  |
| `--face-db-init`     | *(off)*   | create the database schema and exit (no camera)       |
| `--face-db-reembed`  | *(off)*   | recompute the vector of every stored face from its picture (no camera) |
| `--face-db-merge=<from>:<to>` | *(none)* | move the samples of one or more people to another one (no camera) |
| `--face-db-duplicates` | *(off)*  | report the people that look like one person (no camera) |
| `--face-db-duplicates-distance=<f>` | `0.6` | distance below which two people are reported as one person |
| `--face-db-table=<name>` | `face_enrollments` | the table of the PostgreSQL face database |
| `--enroll=<id>`      | *(none)*  | enrol the best face of the first frame with one       |
| `--enroll-name=<name>` | *(none)* | name of the person being enrolled; shown on screen    |
| `--enroll-metadata=<json>` | *(none)* | extra properties of the person, as a JSON document |
| `--auto-enroll`       | *(off)*   | enrol every face that is not in the database yet      |
| `--auto-enroll-mode=<m>` | `deferred` | `deferred` watches the face first, `immediate` stores it at once |
| `--auto-enroll-samples=<n>` | `3`  | views to collect before the person is stored          |
| `--auto-enroll-window=<s>` | `20`   | seconds to collect the views of a face                |
| `--auto-enroll-timeout=<s>` | `10`  | seconds a face may be out of sight before it is forgotten |
| `--auto-enroll-consistency=<f>` | `0.35` | largest distance between two views of the same face  |
| `--auto-enroll-diversity=<f>` | `0.08` | smallest distance between two views of the same face |
| `--auto-enroll-min-size=<px>` | `64` | smallest face that is worth keeping, in pixels        |
| `--auto-enroll-strict`   | *(off)*   | throw a face away instead of storing it with fewer views |
| `--auto-enroll-attach`   | *(off)*   | add the views to a person that is in the database already |
| `--auto-enroll-attach-max=<n>` | `20` | how many samples a person may hold before its views are no longer added to it |
| `--auto-enroll-verbose`  | *(off)*   | print every view that is accepted and rejected        |
| `--face-recognition-model=<path>` | `third_party/face-detection/models/face_recognition_sface_2021dec.onnx` | SFace model file used by `--recognize`/`--enroll` |
| `--face-max-distance=<f>` | `0.4` | largest cosine distance still counted as a match      |
| `--face-same-distance=<f>` | `0.6` | distance above which a face is certainly somebody new |
| `--face-match-samples=<n>` | `3` | how many samples of a person are averaged when a face is matched against the people of the database |
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
>
> The RTSP backend decodes with the settings `ffplay` uses for a live
> preview (`-rtsp_transport udp -fflags nobuffer -flags low_delay`) and keeps
> only the newest frame. Measured against a 1080p30 HEVC camera on a
> wired LAN, the frame that reaches the screen is about **45 ms** old — the
> same as `ffplay` with those flags. The OpenCV `VideoCapture` fallback
> needs about **1.7 s**, because it buffers the decoded frames and hands
> them out oldest first.
>
> `--transport=tcp` forces the TCP transport. UDP is the default because a
> lost TCP segment holds the whole stream back until it is retransmitted;
> the backend falls back to TCP on its own when the camera does not accept
> UDP.
>
> FFmpeg's own diagnostics are suppressed by the library, because a camera
> that sends a slightly broken stream makes it complain about every frame.
> Set `OBJECTIVE_CONTROL_FFMPEG_LOG` to `error`, `warning`, `info` or
> `debug` to see what FFmpeg has to say about the stream.

> Adding `--faces` (or `--recognize`) draws a box around every detected face.
> A recognised person gets a green box labelled with their **name** -- or
> with their id when no name was enrolled -- and an unknown face an amber
> box labelled `unknown`. The same overlay is drawn on the
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
| `Ip`       | `rtsp://...`, `http://...` (MJPEG or snapshot) | RTSP: FFmpeg (low latency) or OpenCV; HTTP: built-in client + `imgcodecs` |

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
  faces: 2 (412,180 213x280 score=0.918774) -> Ada Lovelace (person 42, distance 0.0832154) (1244,201 226x296 score=0.909496) -> unknown (distance 0.571002)
Captured 1 frame(s).
Detected 2 face(s) in total.
Recognised 1 face(s) against the database.
```

`--enroll` requires `--face-db`, because an enrolment that is never written
to the store is lost. The database uses the same file format as the
submodule's own app, so both can share one `faces.db`.

An enrolment holds the embedding of the face and the **properties** of the
person it belongs to:

| Property   | How it is set                          | What it is for                                  |
|------------|----------------------------------------|-------------------------------------------------|
| the name   | `--enroll-name=<name>`                 | shown on screen when the person is detected      |
| the picture| automatically, from the enrolled frame  | recognising the person by eye later on           |
| the metadata | `--enroll-metadata=<json>`           | anything else the application wants to remember  |

```sh
./build/debug/bin/objective-control-app \
    --mode=embedded --source=/dev/video0 --enroll=42 \
    --enroll-name="Ada Lovelace" --enroll-metadata='{"role":"admin"}' \
    --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control
```

The metadata has to be a JSON document; anything else is rejected before
the camera is opened.

#### Automatic enrolment (`--auto-enroll`)

`--enroll` has to be told which person is in front of the camera.
`--auto-enroll` does not: it enrols **every face that is not in the database
yet**, so a camera can be left running and the people who show up are
registered by themselves.

```sh
./build/debug/bin/objective-control-app \
    --mode=embedded --source=/dev/video0 --preview \
    --auto-enroll --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control
```

A face is **not** written to the database the moment it is seen. The
application watches it for a while first: it collects the **views** of the
face -- the picture and the embedding of every frame the face shows up in --
and it writes the person down only when the views agree with each other and
the face has been recognised with high confidence. A face that never becomes
confident leaves **nothing** behind.

The reason is that one frame is not enough material to recognise a person
afterwards. SFace embeddings are stable across small changes, but a face
seen once, in one pose, at one distance, is a single point in the embedding
space. A person who is stored with three views is recognised from more than
one angle, and a person who is stored from a single blurry frame is not
recognised at all.

The life of a face that nobody knows:

| The face ...                            | What happens                                     |
|-----------------------------------------|--------------------------------------------------|
| is not in the database yet               | is watched in memory; no row is written           |
| ... and collects enough views           | is stored with **every** view that was collected  |
| ... and is not seen again                | is forgotten; nothing is written                 |
| is in the database, with a name          | nothing is stored; the person is recognised       |
| is in the database, without a name       | nothing is stored; the person is recognised       |

The last two rows are the point of the mode: a face that was enrolled
automatically has no name, and it still counts as being in the database.
The person is recognised from the next frame on, and is not enrolled a
second time.

##### The knobs

| Option                        | Default | What it does                                              |
|-------------------------------|---------|-----------------------------------------------------------|
| `--auto-enroll-mode=<m>`      | `deferred` | `deferred` watches the face first, `immediate` stores it at once |
| `--auto-enroll-samples=<n>`   | `3`     | How many views are collected before the person is stored    |
| `--auto-enroll-window=<s>`    | `20`    | How long the views of a face are collected, in seconds     |
| `--auto-enroll-timeout=<s>`   | `10`    | How long a face may be out of sight before it is forgotten |
| `--auto-enroll-consistency=<f>` | `0.35` | The largest distance between two views of the same face   |
| `--auto-enroll-diversity=<f>` | `0.08`  | The smallest distance between two views of the same face   |
| `--auto-enroll-min-size=<px>` | `64`    | The smallest face that is worth keeping, in pixels         |
| `--auto-enroll-strict`        | *(off)* | Throw a face away instead of storing it with the views it has |
| `--auto-enroll-attach`        | *(off)* | Add the views to a person that is in the database already  |
| `--auto-enroll-attach-max`     | `20`    | How many samples a person may hold before the faces that look like it are no longer added to it. `0` means no limit |
| `--auto-enroll-verbose`       | *(off)* | Print every view that is accepted and rejected            |

A view only becomes a **view** of a candidate when it passes the gates:

| Gate        | Rule                                                            |
|-------------|------------------------------------------------------------------|
| size        | the face is at least `--auto-enroll-min-size` pixels wide         |
| detection   | the detector is at least as sure as `--face-score`               |
| duplicate   | the view is at least `--auto-enroll-diversity` away from the other views |
| identity    | the view is at most `--auto-enroll-consistency` away from the other views |

The last two pull in opposite directions: the views of a candidate have to
be the same person, but they have to be *different pictures* as well --
otherwise the person is stored with three copies of the same frame. The
defaults leave room in between.

The **confidence** of a candidate is the largest distance between two of its
views: the two views that disagree the most decide how sure the application
is that they show one person. The person is stored when the confidence is at
most `--auto-enroll-consistency` -- that is, when the application can
recognise the face it is watching against its own collected views.

When the collection window is over (`--auto-enroll-window`), a candidate
that holds at least one good view and that is confident enough is stored
with the views it has: a person who stands in front of the camera for two
seconds and leaves is still enrolled, but with one view instead of three.
`--auto-enroll-strict` turns that into a discard.

##### What is stored

A person that was enrolled automatically is stored with one row per view:

| Column      | Value                                                      |
|-------------|------------------------------------------------------------|
| `person_id` | the id that was handed out when the person was stored        |
| `embedding` | the embedding of the view                                    |
| `user_name` | empty -- the name is filled in later, by the editor          |
| `photo`     | the picture of the view                                      |
| `metadata`  | `{"auto_enrolled": true, "views": 3, "confidence": 0.21}`     |

The metadata is a property of the **person** and not of a single view, so
it is stored with every view of that person -- which is what the face
database editor expects. The views of one person are written in one go, so
that the person is either stored with all of them or not at all.

The id of a new person is one that has not been used yet. The ids in
the database are read back from the store before one is handed out, so that a
second instance of the application does not pick an id that somebody else has
taken in the meantime. The ids are handed out in increasing order and are
never reused, so an id always means the same person -- an id that was
deleted does not turn up again with somebody else behind it. An id is only
handed out when the person is stored, so a face that is never confirmed does
not burn one.

A new person is **not** called `unknown`:

- their id is the number the app shows on screen (`person 42`);
- their name is left empty on purpose -- it is filled in later with the
  [face database editor](#reviewing-the-enrolments);
- their metadata says where they came from: `{"auto_enrolled": true}`;
- the picture of the face is stored with every sample, like any other
  enrolment.

##### What the operator sees

The face that is being watched is drawn in blue, with the number of views
it has, so that it is visible that the application is still deciding:

```
Frame 0: 1920x1080 (3 ch), 6220800 bytes, t=6454us.
  faces: 1 (412,180 213x280 score=0.918774) -> tracking a new face (1 view)
  Tracking a new face (candidate 1).
Frame 1: 1920x1080 (3 ch), 6220800 bytes, t=71204us.
  faces: 1 (412,180 213x280 score=0.918774) -> tracking a new face (2 views)
Frame 2: 1920x1080 (3 ch), 6220800 bytes, t=138220us.
  faces: 1 (412,180 213x280 score=0.918774) -> tracking a new face (3 views)
  Enrolled person 1 with 3 view(s) (confidence 0.21).
```

and at the end of the run:

```
Tracked 12 face(s); 9 were stored, 3 were discarded.
Enrolled 9 new person(s) with 27 view(s) in postgres://objective_control:objective_control@127.0.0.1:5432/objective_control.
```

A candidate that was thrown away says why:

```
  Candidate 4 discarded: 1 view(s), the face was out of sight for too long.
```

##### The old behaviour (`--auto-enroll-mode=immediate`)

`--auto-enroll-mode=immediate` stores a face as soon as it is seen, the way
the earlier versions did: one detection is one row, one embedding and one
picture, whatever the quality of that single frame. It is the fallback for
a deployment that wants the old behaviour, and it is what
`--auto-enroll-samples=1` amounts to with the quality gate in front of it.

##### A face that is probably somebody who is enrolled already

The camera sees a face differently every time: another pose, another
distance, a little less light. A face that was seen in another session is
therefore not close enough to any *single* sample of that person to be
recognised, even though the person as a whole agrees with it.

The application scores a face against the **people** of the database, not
against the single samples: the score of a person is the mean distance to
the few of its samples that are the closest to the face. That score is
steadier than the distance to the closest sample, and it is what tells a
person who was enrolled twice apart from a person who is merely unknown.

A face whose score is within `--face-same-distance` is *probably*
somebody who is in the database already. Such a face is not a person of
its own: its views are **added to the person it belongs to**, so that the
person gets more material to be recognised with. A person that is
described by enough pictures does not need every sighting of it, and
`--auto-enroll-attach-max` is where that is decided.

##### Enriching the people that are enrolled already (`--auto-enroll-attach`)

A person that was enrolled by hand with a single, bad sample stays bad.
With `--auto-enroll-attach`, the views that are collected for a face that
matches such a person are added to that person instead of being thrown
away, so the person gets more resources and the recognition improves.

It is **off** by default, because it writes to people that were enrolled by
hand. The views are only added while the person holds fewer than
`--auto-enroll-samples` samples -- without that limit, a person who stands
in front of the camera would add three rows to the database every second.

`--auto-enroll` needs `--face-db`, because a face that is never written to
the store is lost, and it cannot be combined with `--enroll`: one of the two
names the person to enrol, the other enrols whoever shows up.

> A face that is **further** than `--face-max-distance` away from every
> sample is treated as a new person and is enrolled again. Lower the
> threshold to be stricter, raise it when the same person is enrolled twice.
>
> A face that is *not* that far away -- between `--face-max-distance` and
> `--face-same-distance` -- is very likely somebody who is enrolled already:
> the camera sees a face differently every time (another pose, another
> distance, a little less light), and enrolling such a face again would
> fill the database with copies of the same people. It is reported as
> `probably <name>` and is **not** enrolled again. Its views are added to
> that person all the same, which is what makes the person easier to
> recognise the next time. Lower `--face-same-distance` when new people
> are not registered any more, raise it when they are.
>
> `--face-match-samples` says how many samples of a person are averaged
> when the face is matched against the people of the database. One is the
> distance to the closest sample; more than one is steadier. Three is a
> good compromise: it smooths the noise of a single sample without hiding
> a person behind the others.

### Where the enrolments are stored

`--face-db` takes a **target**, and the target decides where the enrolments
live:

| Target                            | Backend                                    |
|-----------------------------------|--------------------------------------------|
| `faces.db`                        | a file, in the flat `FDB2` binary format    |
| `postgres://user@host/database`   | a PostgreSQL table                          |

A target that starts with `postgres://` or `postgresql://` is a PostgreSQL
one; everything else is a file. When `--face-db` is not given, the target is
taken from `$FACE_DB_URL` or `$DATABASE_URL`, so a deployment does not have
to repeat it on every command line:

```sh
export DATABASE_URL='postgres://objective_control:objective_control@127.0.0.1:5432/objective_control'
```

#### PostgreSQL

The PostgreSQL store keeps one row per enrolled sample:

```sql
CREATE TABLE face_enrollments (
    id          bigserial   PRIMARY KEY,
    person_id   integer     NOT NULL,
    embedding   real[]      NOT NULL,
    user_name   text,
    photo       bytea,
    metadata    jsonb       NOT NULL DEFAULT '{}'::jsonb,
    enrolled_at timestamptz NOT NULL DEFAULT now()
);
```

The three optional columns hold the properties of the person: the name
that is shown on screen (`user_name`), a picture of the face (`photo`) and
an arbitrary JSON document (`metadata`). They are stored with **every**
sample of a person, so the store can answer "who is this?" without a second
lookup.

Several rows may share a `person_id`: a person who was enrolled
automatically is stored with one row per view of the face. Those rows are
written in **one transaction**, so that a person is either stored with all
of its views or not at all.

The columns were added after the table was introduced, so a table that
predates them is **migrated in place** when the app connects -- the rows
that are already there keep their data. A table that the runtime user is
not allowed to alter is used as it is, without the properties: the app
opens the store anyway and prints a warning that names the columns that
are missing, so that it is clear why the name of a person is not stored.
`--face-db-init` reports the problem and exits with status `1`, because
the schema was asked for explicitly.

The table is created when the app connects, so a plain database is enough.
The DDL is also in
[`third_party/face-detection/sql/schema.sql`](third_party/face-detection/sql/schema.sql)
for deployments that prepare the schema separately. The connection uses
libpq, so `PGHOST`, `PGPORT`, `PGDATABASE`, `PGUSER` and `PGPASSWORD` apply
whenever the connection string does not spell them out.

The table name is the one in `PostgresFaceStore::Options::table`
(`face_enrollments` by default). PostgreSQL truncates identifiers to 63
bytes, so the store shortens the name of the index it derives from it
before it is sent -- a name that the server shortens would no longer match
the name the store is looking for.

Set the database up with the helper script (it needs a PostgreSQL
superuser):

```sh
# Creates the role and the database, then applies the schema
scripts/setup-db.sh --db-name=objective_control --db-user=objective_control

# ... or with an explicit superuser connection
scripts/setup-db.sh --admin-dsn='host=127.0.0.1 user=postgres dbname=postgres'

# ... or with a superuser URL
scripts/setup-db.sh \
    --admin-dsn='postgres://postgres:postgres@127.0.0.1:5432/postgres' \
    --db-name=objective_control --db-user=objective_control \
    --db-password=objective_control
```

Without `--admin-dsn` the script uses `sudo -u postgres psql`, which is
the usual way on a Debian/Ubuntu machine. The script can be run again on
an existing installation: the role and the database are only created when
they are missing.

The connection string the script prints is the one to hand to the
application:

```sh
export DATABASE_URL='postgres://objective_control:objective_control@127.0.0.1:5432/objective_control'
```

Enrol into PostgreSQL and list who is enrolled:

```sh
./build/debug/bin/objective-control-app \
    --mode=embedded --source=/dev/video0 --enroll=42 \
    --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control

./build/debug/bin/objective-control-app \
    --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control --face-db-list

./build/debug/bin/objective-control-app \
    --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control --face-db-remove=42
```

The three `--face-db-*` options work on a file too, and none of them opens a
camera.

Enrolling a person **appends** one row; the samples of the other people are
left alone. That keeps the enrolment cheap and makes it safe to enrol from
several machines at once. A person that is enrolled automatically with
several views appends one row per view, in one transaction.

`--face-max-distance` is the cosine-distance cut-off for a match. The default
of `0.4` is the usual "same person" threshold for SFace; lower it to be
stricter, raise it if legitimate matches are being rejected.

That default assumes a face that fills a good part of the frame. A face
that is only a few dozen pixels wide is blurry, and the embedding of such a
face moves by 0.1 to 0.3 between two pictures of the same person -- more
than the threshold itself. Two things help there: a camera that is closer
to the subject, and `--face-detect-width=0`, which runs the detector at the
resolution of the frame. The landmarks are then measured on the pixels
themselves, and the embedding that is computed from them is steadier.

#### Finding the people that were enrolled twice

A face that was enrolled twice -- in two sessions, with another light and
another distance -- ends up as two people in the database. The picture of
a person does not tell the two apart: they look like one person, which is
exactly why the application could not keep them together.

`--face-db-duplicates` reports the people that look like one person, as
groups: the people that are close enough to each other end up in one
group, even when they only meet through a third one.

```sh
./build/debug/bin/objective-control-app \
    --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control \
    --face-db-duplicates --face-db-duplicates-distance=0.65
```

```
  person 1 (10 sample(s), Bushko, person 9 (1 sample(s), person 10 (3 sample(s), ...
      person 9 is at distance 0.452 from person 1
      person 10 is at distance 0.56 from person 1
      person 11 is at distance 0.486 from person 1
```

The distance below which two people are reported is
`--face-db-duplicates-distance` (`0.6` by default). It is the distance
between the *people*, not between two of their samples: it is the mean of
the smallest distances between the samples of the two people, which is
steadier than the distance to a single sample.

`--face-db-merge` puts the people of such a group back together. The
rows keep their identity and the time the face was enrolled; only the
person they belong to changes:

```sh
./build/debug/bin/objective-control-app \
    --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control \
    --face-db-merge=9,10,13:1
```

The "from" part may hold several ids, separated by commas. The metadata
of the samples that are moved says where they came from, so that the
origin of a sample is not lost:

```json
{"auto_enrolled": true, "merged_from_person_id": 9}
```

> `--face-db-table=<name>` names another table of the PostgreSQL
> database. It is what makes it possible to look at a copy of the
> database without touching the real one -- and to try a merge out
> before it is done for real.

#### The size of the face image (`--face-image-size`)

The face is cut out of the frame, and the cut-out is what is stored with
the enrolment and what the recognizer works on. `--face-image-size=<px>`
sets how large that cut-out is: a face that is smaller than the given size
is scaled up to it, and a face that is already bigger is left alone. The
default is 320 pixels, which is the size of a picture of a face that is
worth looking at; `0` keeps the size the face had in the frame.

The same image is used for both, so that the picture in the editor is
exactly the one the face was recognised by.

Scaling a face up does not invent detail -- it cannot make a face that
was far away as good as one that was close. It makes the stored picture
usable, and it keeps the picture and the embedding in step. A face that is
recognised badly is a face that had too few pixels in the frame: raise
`--width`/`--height` for that, and lower `--face-detect-width` towards 0
so that the landmarks are measured on the pixels themselves.

#### Reviewing the enrolments

The properties of a person are written once, when the person is enrolled,
and are rarely right afterwards: a name gets misspelled, a piece of
metadata turns out to be missing, the wrong picture was captured. The
**face database editor** opens the same table and lets them be corrected.

It is a small Python application with a window, in
[`tools/face-db-editor`](tools/face-db-editor):

```sh
pip install -r tools/face-db-editor/requirements.txt

python3 tools/face-db-editor/face_db_editor.py \
    --dsn='postgres://objective_control:objective_control@127.0.0.1:5432/objective_control'
```

The connection string is taken from `--dsn`, else from `$FACE_DB_URL`, else
from `$DATABASE_URL` -- the same order the app uses. It needs a display.

The editor lists the enrolled people, shows the samples of the selected one
with the picture of the face that was captured at the time, and stores a
corrected name or metadata again. The picture can be saved to a file. The
embedding of the face -- the vector the recognition model computed for it
-- is shown next to the samples, for the sake of looking at it. The
changes are written to every sample of the person, exactly as the app
writes them when it enrols somebody, so the app picks them up the next time
it loads the store.

The editor can also **merge** people by hand. A face that was enrolled
twice -- in two sessions, with another light and another distance -- ends
up as two people in the table, and the **Merge person...** button puts
such a pair back together: the samples of the people that are picked are
moved to the person that is selected, which keeps its id. The rows keep
their identity and the time the face was enrolled, and the metadata of a
moved sample says where it came from. It is the same thing that
[`--face-db-merge`](#finding-the-people-that-were-enrolled-twice) does on
the command line, only by hand.

See [`tools/face-db-editor/README.md`](tools/face-db-editor/README.md) for
the details.

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
| `OBJECTIVE_CONTROL_ENABLE_FFMPEG`      | `ON`    | Low-latency RTSP backend            |
| `OBJECTIVE_CONTROL_ENABLE_GSTREAMER`    | `OFF`   | GStreamer pipelines (future)        |
| `OBJECTIVE_CONTROL_BUILD_APPS`          | `ON`    | Build the example app               |
| `OBJECTIVE_CONTROL_BUILD_TESTS`         | `ON`    | Build + register tests              |
| `OBJECTIVE_CONTROL_ENABLE_SANITIZERS`   | `OFF`   | ASan/UBSan in debug builds          |
| `OBJECTIVE_CONTROL_ENABLE_FACE`         | `OFF`   | Use the face-detection submodule    |
| `OBJECTIVE_CONTROL_ENABLE_POSTGRESQL`   | `ON`    | Store the enrolled faces in PostgreSQL |

## Layout

```
include/objective-control/camera/camera.hpp   Public API (Camera, Frame, ...)
include/objective-control/face/auto_enroller.hpp
                                               Tracker of the unknown faces
src/                                          Library implementation
src/backends/                                 Per-backend implementations
src/backends/http_mjpeg.hpp                   Self-contained HTTP MJPEG/snapshot client
src/backends/rtsp_ffmpeg.cpp                  Low-latency RTSP backend (FFmpeg)
src/face/auto_enroller.cpp                    The tracker itself
apps/camera_demo.cpp                          Example CLI application
apps/mjpeg_stream.hpp                         Minimal MJPEG-over-HTTP server
scripts/setup-db.sh                           Creates the PostgreSQL role and database
tools/face-db-editor/                         Desktop editor for the face database
tests/camera_test.cpp                         Smoke test (no external test deps)
tests/auto_enroller_test.cpp                  Tests of the tracker
tests/auto_enroll_integration_test.cpp        Tracker and store together
tests/auto_enroll_cli_test.cmake              Test of --auto-enroll against the app
cmake/                                        Dependency + warning helpers
docs/proposals/                               Design notes of the bigger decisions
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

## Design notes

The decisions that are too big to be explained in a comment are
written down as proposals, in [`docs/proposals`](docs/proposals):

| Proposal | Status | About |
|---------|--------|-------|
| [`0001-deferred-auto-enrolment`](docs/proposals/0001-deferred-auto-enrolment.md) | implemented | Why `--auto-enroll` watches a face before it writes it down, how the tracker decides that two views show the same person, and what ends up in the database. |

The behaviour the proposal describes is the one of
[Automatic enrolment](#automatic-enrolment---auto-enroll); the older way
to do it is still there as `--auto-enroll-mode=immediate`.

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
