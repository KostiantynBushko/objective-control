#include "objective-control/camera/camera.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef OC_HAVE_OPENCV
#include <opencv2/highgui.hpp>
#include <opencv2/videoio.hpp>
#include "mjpeg_stream.hpp"
#endif

#ifdef OC_HAVE_FACE
#include "face-detection/face/detector.hpp"
#endif

namespace {
// Set from the SIGINT/SIGTERM handler. It must be lock-free so that storing
// from a signal handler is safe.
std::atomic<bool> g_stop{false};

// SIGINT/SIGTERM handler: ask the loops to finish so the camera and the
// sockets are released cleanly instead of being torn down mid-frame.
void onStopSignal(int) {
    g_stop.store(true);
}

// Parses "native" or "<width>x<height>" into an explicit window size.
// Returns false when the spec requests native sizing or is malformed.
bool parseWindowSize(const std::string& spec, int& width, int& height) {
    if (spec.empty() || spec == "native") {
        return false;
    }
    const auto x = spec.find_first_of("xX");
    if (x == std::string::npos) {
        return false;
    }
    try {
        width = std::stoi(spec.substr(0, x));
        height = std::stoi(spec.substr(x + 1));
    } catch (const std::exception&) {
        return false;
    }
    return width > 0 && height > 0;
}

// True if a bare "--<name>" flag is present (no value).
bool flag(int argc, char** argv, const std::string& name) {
    const std::string needle = "--" + name;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == needle) {
            return true;
        }
    }
    return false;
}

// Tiny argument helper: returns the value of --<name>= or a fallback.
std::string arg(int argc, char** argv, const std::string& name,
                const std::string& fallback) {
    const std::string prefix = "--" + name + "=";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind(prefix, 0) == 0) {
            return a.substr(prefix.size());
        }
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Command-line help and validation
// ---------------------------------------------------------------------------

// Options that are switches: they never take a value.
bool isBooleanOption(const std::string& name) {
    return name == "preview" || name == "window-fullscreen" ||
           name == "faces" || name == "help";
}

// Every option the app understands. Anything else is a typo and is reported
// rather than silently ignored.
bool isKnownOption(const std::string& name) {
    static const char* const kOptions[] = {
        "mode",    "source", "width",       "height", "fps",
        "frames",  "seconds", "out",       "record", "stream",
        "preview", "window-size", "window-fullscreen", "help",
        "faces",   "face-model",
    };
    for (const char* known : kOptions) {
        if (name == known) {
            return true;
        }
    }
    return false;
}

// True when "-h" or "--help" was requested.
bool wantsHelp(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            return true;
        }
    }
    return false;
}

// Rejects unknown options and stray arguments. Every option carries its
// value with '=' (--width=640), so a standalone word is always a mistake.
bool validateArgs(int argc, char** argv) {
    bool ok = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

        if (a == "-h" || a == "--help") {
            continue;
        }

        if (a.rfind("--", 0) != 0) {
            if (a.rfind("-", 0) == 0) {
                std::cerr << "Unknown option '" << a << "'.\n";
            } else {
                std::cerr << "Unexpected argument '" << a
                          << "' (options take the form --name=value).\n";
            }
            ok = false;
            continue;
        }

        const auto eq = a.find('=');
        const std::string name =
            (eq == std::string::npos) ? a.substr(2) : a.substr(2, eq - 2);

        if (!isKnownOption(name)) {
            std::cerr << "Unknown option '" << a << "'.\n";
            ok = false;
        } else if (eq != std::string::npos && isBooleanOption(name)) {
            std::cerr << "Option '--" << name
                      << "' is a switch and takes no value.\n";
            ok = false;
        }
    }
    return ok;
}

// Prints the usage text. Mirrored by the "Command-line app" section of
// README.md, so keep the two in sync.
void printUsage(std::ostream& out) {
    out << R"USAGE(objective-control-app - capture, record and stream camera frames

Usage:
  objective-control-app [options]

Options:
  --mode=<mode>          Camera backend (default: null):
                           null       no-op placeholder
                           embedded  local USB/CSI/MIPI device
                           ip        network camera (RTSP / HTTP)
                           synthetic built-in moving test pattern
  --source=<source>      Source for the backend. For --mode=embedded a device
                         path or index (/dev/video0, 0); for --mode=ip a URL
                         (rtsp://host:554/stream, http://host/?action=stream,
                         http://host/snapshot.jpg).
  --width=<px>           Requested frame width (default: 1920)
  --height=<px>          Requested frame height (default: 1080)
  --fps=<n>              Requested frame rate (default: 30)
  --frames=<n>           Number of frames to grab (default: 1)
  --seconds=<n>          Recording duration in seconds (default: 5)
  --out=<path>           Save the first captured frame as a binary PPM (P6)
  --record=<path>        Record video to <path>; the extension picks the
                         codec (.mp4 or .avi)
  --stream=<port>        Serve MJPEG over HTTP on <port>; open
                         http://localhost:<port>/ in a browser. Runs until
                         Ctrl+C. Several clients may watch at once.
  --preview              Open a live preview window; press 'q' or Esc to quit
  --window-size=<size>   Preview window size: native (default) or <width>x<height>
  --window-fullscreen    Make the preview window fullscreen
  --faces                Run face detection on each captured frame and
                         report how many faces were found (requires a build
                         with OBJECTIVE_CONTROL_ENABLE_FACE=ON)
  --face-model=<path>    YuNet model file used by --faces
                         (default: third_party/face-detection/models/
                          face_detection_yunet_2023mar.onnx)
  -h, --help             Show this help and exit

Options take their value with '=' (e.g. --width=640).

Examples:
  # Built-in test pattern, no hardware needed
  objective-control-app --mode=synthetic --width=640 --height=480

  # Save one frame as a PPM image
  objective-control-app --mode=embedded --source=/dev/video0 --out=frame.ppm

  # Grab 10 frames and print their size
  objective-control-app --mode=embedded --source=0 --frames=10

  # Record five seconds to MP4
  objective-control-app --mode=embedded --source=/dev/video0 --fps=30 \
      --seconds=5 --record=clip.mp4

  # Stream over HTTP (open http://localhost:8080/)
  objective-control-app --mode=ip --source=rtsp://host:554/stream --stream=8080

  # Live preview at 1280x720
  objective-control-app --mode=embedded --source=/dev/video0 --preview \
      --window-size=1280x720

  # Fullscreen preview of an IP camera
  objective-control-app --mode=ip --source=http://host/?action=stream \
      --preview --window-fullscreen

  # Detect faces in ten frames from a USB camera
  objective-control-app --mode=embedded --source=/dev/video0 --frames=10 --faces

Exit codes:
  0  success
  1  runtime error (camera could not be opened, no frames captured, ...)
  2  bad usage (unknown option, invalid mode)
)USAGE";
}

// Writes a binary PPM (P6) image. Frames are BGR, PPM expects RGB.
bool savePpm(const oc::Frame& frame, const std::string& path) {
    if (frame.channels != 3) {
        std::cerr << "PPM save supports 3-channel (BGR) frames only.\n";
        return false;
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "Cannot open '" << path << "' for writing.\n";
        return false;
    }

    out << "P6\n" << frame.width << " " << frame.height << "\n255\n";
    for (std::size_t i = 0; i < frame.data.size(); i += 3) {
        out.put(static_cast<char>(frame.data[i + 2])); // R (was B)
        out.put(static_cast<char>(frame.data[i + 1])); // G
        out.put(static_cast<char>(frame.data[i + 0])); // B (was R)
    }
    return static_cast<bool>(out);
}

#ifdef OC_HAVE_OPENCV
// Pick a widely-supported codec from the output file extension.
int fourccForPath(const std::string& path) {
    std::string ext;
    const auto dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        for (auto& c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (ext == ".avi") {
        return cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
    }
    return cv::VideoWriter::fourcc('m', 'p', '4', 'v'); // MPEG-4 Part 2
}

// Wrap a Frame's payload (contiguous, BGR or gray) in a cv::Mat view.
// Returns false for channel counts we don't handle.
bool toMat(const oc::Frame& frame, cv::Mat& out) {
    if (frame.channels == 1) {
        out = cv::Mat(frame.height, frame.width, CV_8UC1,
                      const_cast<std::uint8_t*>(frame.data.data()));
        return true;
    }
    if (frame.channels == 3) {
        out = cv::Mat(frame.height, frame.width, CV_8UC3,
                      const_cast<std::uint8_t*>(frame.data.data()));
        return true;
    }
    return false;
}

bool writeFrame(cv::VideoWriter& writer, const oc::Frame& frame) {
    cv::Mat image;
    if (!toMat(frame, image)) {
        return false;
    }
    writer.write(image);
    return true;
}
#endif

} // namespace

static_assert(std::atomic<bool>::is_always_lock_free,
              "the signal handler requires a lock-free atomic");

int main(int argc, char** argv) {
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    // --help wins over everything else, including a bad option elsewhere.
    if (wantsHelp(argc, argv)) {
        printUsage(std::cout);
        return 0;
    }
    if (!validateArgs(argc, argv)) {
        std::cerr << "Run with --help for the list of options.\n";
        return 2;
    }

    const std::string mode = arg(argc, argv, "mode", "null");
    const std::string source = arg(argc, argv, "source", "");
    const std::string outPath = arg(argc, argv, "out", "");
    const std::string recordPath = arg(argc, argv, "record", "");
    const int streamPort = std::atoi(arg(argc, argv, "stream", "0").c_str());
    const bool preview = flag(argc, argv, "preview");
    const bool fullscreen = flag(argc, argv, "window-fullscreen");
    const std::string framesStr = arg(argc, argv, "frames", "1");
    const int frameCount = std::max(1, std::atoi(framesStr.c_str()));
    const double seconds = std::max(0.0, std::atof(arg(argc, argv, "seconds", "5").c_str()));
    const bool detectFaces = flag(argc, argv, "faces");
    const std::string faceModel = arg(argc, argv, "face-model",
                                      "third_party/face-detection/models/"
                                      "face_detection_yunet_2023mar.onnx");

    int windowWidth = 0;
    int windowHeight = 0;
    const std::string windowSizeSpec = arg(argc, argv, "window-size", "native");
    if (!parseWindowSize(windowSizeSpec, windowWidth, windowHeight)) {
        if (windowSizeSpec != "native" && !windowSizeSpec.empty()) {
            std::cerr << "Ignoring invalid --window-size='" << windowSizeSpec
                      << "' (expected 'native' or '<width>x<height>').\n";
        }
    }

    oc::CameraConfig config;
    config.width = std::atoi(arg(argc, argv, "width", "1920").c_str());
    config.height = std::atoi(arg(argc, argv, "height", "1080").c_str());
    config.fps = std::atoi(arg(argc, argv, "fps", "30").c_str());

    if (mode == "embedded") {
        config.kind = oc::CameraKind::Embedded;
    } else if (mode == "ip") {
        config.kind = oc::CameraKind::Ip;
    } else if (mode == "synthetic") {
        config.kind = oc::CameraKind::Synthetic;
    } else if (mode != "null") {
        std::cerr << "Unknown mode '" << mode
                  << "'. Use one of: null, embedded, ip, synthetic.\n";
        return 2;
    }
    config.source = source;

    auto camera = oc::Camera::create(config.kind);
    if (!camera) {
        std::cerr << "Backend for '" << mode
                  << "' is not available in this build.\n";
        return 1;
    }

    if (!camera->open(config)) {
        std::cerr << "Failed to open camera";
        if (!config.source.empty()) {
            std::cerr << " at '" << config.source << "'";
        }
        std::cerr << ".\n";
        return 1;
    }

    // ---------------- Live streaming / preview -------------
    if (streamPort > 0 || preview) {
#ifdef OC_HAVE_OPENCV
        const auto period = std::chrono::microseconds(
            1000000 / std::max(1, config.fps));
        auto nextFrame = std::chrono::steady_clock::now();

        // Grab -> throttle to ~fps -> emit as a BGR cv::Mat. Returns false
        // once stop is requested or the camera stops producing frames.
        auto grabFrame = [&](cv::Mat& out) -> bool {
            while (!g_stop.load()) {
                auto frame = camera->grab();
                if (frame) {
                    cv::Mat view;
                    if (toMat(*frame, view)) {
                        // Deep-copy: the grabbed Frame is destroyed when this
                        // lambda returns, so the cv::Mat must own its data.
                        out = view.clone();
                        std::this_thread::sleep_until(nextFrame);
                        nextFrame = std::chrono::steady_clock::now() + period;
                        return true;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            return false;
        };

        if (streamPort > 0) {
            std::cout << "Streaming to http://localhost:" << streamPort
                      << "/  (Ctrl+C to stop)\n";
            apps::MjpegServer server;
            if (!server.serve(streamPort, grabFrame, g_stop)) {
                std::cerr << "Failed to start stream on port " << streamPort
                          << ".\n";
                camera->close();
                return 1;
            }
            camera->close();
            return 0;
        }

        // Live preview window (requires a display).
        std::cout << "Live preview (press 'q' or Esc to quit)\n";
        const std::string kWindowTitle = "objective-control: camera";
        // WINDOW_NORMAL keeps the window resizable; without it the window
        // manager tends to scale the frame to the whole screen.
        cv::namedWindow(kWindowTitle, cv::WINDOW_NORMAL);

        cv::Mat image;
        bool windowSized = false;
        while (!g_stop.load()) {
            if (!grabFrame(image)) {
                break;
            }
            cv::imshow(kWindowTitle, image);
            if (!windowSized) {
                // Size the window to the frame itself so the picture is
                // neither stretched nor cropped.
                const int w = windowWidth > 0 ? windowWidth : image.cols;
                const int h = windowHeight > 0 ? windowHeight : image.rows;
                cv::resizeWindow(kWindowTitle, w, h);
                if (fullscreen) {
                    cv::setWindowProperty(kWindowTitle, cv::WND_PROP_FULLSCREEN,
                                          cv::WINDOW_FULLSCREEN);
                }
                windowSized = true;
            }
            const int key = cv::waitKey(1) & 0xff;
            if (key == 'q' || key == 27) {
                break;
            }
        }
        cv::destroyAllWindows();
        camera->close();
        return 0;
#else
        std::cerr << "Streaming/preview requires OpenCV (not available).\n";
        camera->close();
        return 1;
#endif
    }

    // ---------------- Video recording path ----------------
    if (!recordPath.empty()) {
#ifdef OC_HAVE_OPENCV
        const int targetFrames = std::max(
            1, static_cast<int>(std::round(seconds * config.fps)));

        cv::VideoWriter writer;
        int written = 0;
        bool writerReady = false;

        // Grab until we hit the requested duration or the camera gives up.
        const int attemptsCap = targetFrames * 100;
        for (int attempt = 0; attempt < attemptsCap; ++attempt) {
            auto frame = camera->grab();
            if (!frame) {
                continue;
            }

            if (!writerReady) {
                writer.open(recordPath, fourccForPath(recordPath),
                            static_cast<double>(config.fps),
                            cv::Size(frame->width, frame->height));
                if (!writer.isOpened()) {
                    std::cerr << "Failed to open '" << recordPath
                              << "' for recording.\n";
                    camera->close();
                    return 1;
                }
                writerReady = true;
            }

            if (!writeFrame(writer, *frame)) {
                std::cerr << "Unsupported frame channel count ("
                          << frame->channels << "); aborting.\n";
                break;
            }

            if (++written >= targetFrames) {
                break;
            }
        }

        writer.release();
        camera->close();

        if (written == 0) {
            std::cerr << "No frames captured; nothing recorded.\n";
            return 1;
        }
        std::cout << "Recorded " << written << " frame(s) ("
                  << (static_cast<double>(written) / config.fps)
                  << " s) to " << recordPath << ".\n";
        return 0;
#else
        std::cerr << "Recording requires OpenCV (not available in this build).\n";
        camera->close();
        return 1;
#endif
    }

    // ---------------- Face detection (optional submodule) ----------------
#ifdef OC_HAVE_FACE
    std::unique_ptr<fd::FaceDetector> faceDetector;
    if (detectFaces) {
        faceDetector = std::make_unique<fd::FaceDetector>();
        fd::FaceDetector::Options faceOptions;
        faceOptions.model_path = faceModel;
        if (!faceDetector->open(faceOptions)) {
            std::cerr << "Could not load the face-detection model '"
                      << faceModel << "'.\nRun "
                         "third_party/face-detection/models/fetch.sh to "
                         "download it.\n";
            camera->close();
            return 1;
        }
        std::cout << "Face detection enabled (model: " << faceModel << ").\n";
    }
#else
    if (detectFaces) {
        std::cerr << "--faces requires a build with "
                     "OBJECTIVE_CONTROL_ENABLE_FACE=ON.\n";
        camera->close();
        return 1;
    }
#endif

    // ---------------- Single/limited frame-capture path ----------------
    int received = 0;
    int facesSeen = 0;
    for (int attempts = 0; attempts < frameCount * 40; ++attempts) {
        auto frame = camera->grab();
        if (!frame) {
            continue;
        }

        std::cout << "Frame " << received << ": " << frame->width << 'x'
                  << frame->height << " (" << frame->channels
                  << " ch), " << frame->data.size() << " bytes, t="
                  << frame->timestamp_us << "us.\n";

#ifdef OC_HAVE_FACE
        if (faceDetector) {
            // oc::Frame and fd::Image share the same dense BGR layout, so
            // the frame can be handed over without copying any pixels.
            fd::Image image;
            image.width = frame->width;
            image.height = frame->height;
            image.channels = frame->channels;
            image.data = frame->data;

            const std::vector<fd::FaceBox> faces = faceDetector->detect(image);
            facesSeen += static_cast<int>(faces.size());
            std::cout << "  faces: " << faces.size();
            for (const fd::FaceBox& face : faces) {
                std::cout << " (" << face.x << "," << face.y << " "
                          << face.width << "x" << face.height << " score="
                          << face.score << ")";
            }
            std::cout << '\n';
        }
#endif

        if (!outPath.empty() && received == 0) {
            if (savePpm(*frame, outPath)) {
                std::cout << "Saved first frame to " << outPath << ".\n";
            }
        }

        if (++received >= frameCount) {
            break;
        }
    }

    camera->close();

    if (received == 0) {
        std::cerr << "No frame received.\n";
        return 1;
    }
    std::cout << "Captured " << received << " frame(s).\n";
#ifdef OC_HAVE_FACE
    if (faceDetector) {
        std::cout << "Detected " << facesSeen << " face(s) in total.\n";
    }
#else
    (void)facesSeen;
#endif
    return 0;
}
