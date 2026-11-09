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
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include "mjpeg_stream.hpp"
#endif

#ifdef OC_HAVE_FACE
#include "face-detection/face/database.hpp"
#include "face-detection/face/detector.hpp"
#include "face-detection/face/recognizer.hpp"
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
           name == "faces" || name == "recognize" || name == "help";
}

// Every option the app understands. Anything else is a typo and is reported
// rather than silently ignored.
bool isKnownOption(const std::string& name) {
    static const char* const kOptions[] = {
        "mode",    "source", "width",       "height", "fps",
        "frames",  "seconds", "out",       "record", "stream",
        "preview", "window-size", "window-fullscreen", "help",
        "faces",   "face-model", "face-score", "face-detect-width",
        "recognize", "enroll", "face-db", "face-recognition-model",
        "face-max-distance",
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
                         Ctrl+C. Several clients may watch at once. Always
                         serves the newest frame the camera sent.
  --preview              Open a live preview window; press 'q' or Esc to quit.
                         Always shows the newest frame the camera sent
  --window-size=<size>   Preview window size: native (default) or <width>x<height>
  --window-fullscreen    Make the preview window fullscreen
  --faces                Run face detection on each captured frame and
                         report how many faces were found. With --preview
                         or --stream the detected faces are also drawn onto
                         the image (requires a build with
                         OBJECTIVE_CONTROL_ENABLE_FACE=ON)
  --face-model=<path>    YuNet model file used by --faces
                         (default: third_party/face-detection/models/
                          face_detection_yunet_2023mar.onnx)
  --face-score=<f>       Minimum detection confidence to report a face
                         (default: 0.6; lower catches smaller faces)
  --face-detect-width=<px>
                         Width the detector runs at; the frame is scaled
                         down to it, so detection costs less. 0 means native
                         resolution (default: 960)
  --recognize            Identify every detected face against the enrolled
                         set and print the person id; with --preview or
                         --stream the person id is drawn next to the box
                         (implies --faces)
  --face-db=<path>       Face database file. Loaded when it exists and
                         written back when --enroll is used.
  --enroll=<id>          Enrol the most confident face of the first frame
                         that contains one, under this person id. Implies
                         --faces and requires --face-db.
  --face-recognition-model=<path>
                         SFace model file used by --recognize/--enroll
                         (default: third_party/face-detection/models/
                          face_recognition_sface_2021dec.onnx)
  --face-max-distance=<f>
                         Largest cosine distance still counted as a match
                         (default: 0.4; lower is stricter)
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

  # Preview an RTSP camera with the detected faces drawn on the frames
  objective-control-app --mode=ip --source=rtsp://host:554/stream \
      --preview --faces --recognize --face-db=faces.db

  # Detect faces in ten frames from a USB camera
  objective-control-app --mode=embedded --source=/dev/video0 --frames=10 --faces

  # Identify faces against a database of enrolled people
  objective-control-app --mode=embedded --source=/dev/video0 --frames=10 \
      --recognize --face-db=faces.db

  # Enrol the person in front of the camera as person 42
  objective-control-app --mode=embedded --source=/dev/video0 --enroll=42 \
      --face-db=faces.db

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

#ifdef OC_HAVE_FACE
// Copies a cv::Mat into the layout the face library expects. fd::Image owns
// its pixels, so this is one copy per frame; both use the same dense BGR
// layout, so no colour conversion is needed.
fd::Image toFaceImage(const cv::Mat& mat) {
    const cv::Mat src = mat.isContinuous() ? mat : mat.clone();
    fd::Image image;
    image.width = src.cols;
    image.height = src.rows;
    image.channels = src.channels();
    const std::size_t bytes =
        static_cast<std::size_t>(src.total()) * src.elemSize();
    image.data.assign(src.ptr(), src.ptr() + bytes);
    return image;
}

// Draws a box and a caption for every detected face. A recognised person
// gets a green box, an unknown face an amber one, so the overlay can be
// read at a glance.
void annotateFaces(cv::Mat& image, const std::vector<fd::FaceBox>& faces,
                   const std::vector<std::string>& labels) {
    // Scale the stroke and the caption with the frame so the overlay stays
    // readable on both a 320x240 sub-stream and a 4K one.
    const int thickness = std::max(1, image.cols / 400);
    const double fontScale =
        std::max(0.4, static_cast<double>(image.cols) / 1600.0);

    for (std::size_t i = 0; i < faces.size(); ++i) {
        const fd::FaceBox& face = faces[i];
        const std::string label =
            (i < labels.size()) ? labels[i] : std::string();
        const bool known = label.rfind("person ", 0) == 0;

        const cv::Scalar color =
            known ? cv::Scalar(0, 220, 0) : cv::Scalar(0, 165, 255);
        cv::rectangle(image,
                      cv::Rect(face.x, face.y, face.width, face.height),
                      color, thickness);

        if (label.empty()) {
            continue;
        }

        // Caption above the box; flip it below when there is no room.
        int baseline = 0;
        const cv::Size textSize =
            cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, fontScale,
                            thickness, &baseline);
        int textY = face.y - 6;
        if (textY - textSize.height < 0) {
            textY = face.y + textSize.height + 6;
        }
        const int textX = std::max(0, face.x);

        cv::rectangle(image,
                      cv::Rect(textX, textY - textSize.height - 4,
                               textSize.width + 8, textSize.height + 6),
                      color, cv::FILLED);
        cv::putText(image, label, cv::Point(textX + 4, textY - 4),
                    cv::FONT_HERSHEY_SIMPLEX, fontScale, cv::Scalar(0, 0, 0),
                    thickness, cv::LINE_AA);
    }
}
#endif
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
    const double faceScore =
        std::atof(arg(argc, argv, "face-score", "0.6").c_str());
    const int faceDetectWidth =
        std::atoi(arg(argc, argv, "face-detect-width", "960").c_str());
    const bool doRecognize = flag(argc, argv, "recognize");
    const std::string enrollStr = arg(argc, argv, "enroll", "");
    const std::string faceDbPath = arg(argc, argv, "face-db", "");
    const std::string faceRecModel = arg(argc, argv, "face-recognition-model",
                                         "third_party/face-detection/models/"
                                         "face_recognition_sface_2021dec.onnx");
    const double maxDistance =
        std::atof(arg(argc, argv, "face-max-distance", "0.4").c_str());

    // --enroll=<id> names the person to enrol. -1 means "not enrolling".
    int enrollId = -1;
    if (!enrollStr.empty()) {
        try {
            enrollId = std::stoi(enrollStr);
        } catch (const std::exception&) {
            enrollId = -1;
        }
        if (enrollId < 0) {
            std::cerr << "Invalid --enroll value '" << enrollStr
                      << "' (expected a non-negative integer).\n";
            return 2;
        }
        if (faceDbPath.empty()) {
            std::cerr << "--enroll requires --face-db=<path> to store the "
                         "enrolment.\n";
            return 2;
        }
    }

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

    // A preview and an MJPEG stream are live: the consumer is usually
    // slower than the camera (decoding, drawing, JPEG encoding all take
    // time), so the backend has to hand out the newest frame rather than
    // the oldest one it has buffered. A recording keeps every frame, so it
    // stays on the lossless path.
    config.live = preview || streamPort > 0;

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

    // ---------------- Face detection / recognition (optional submodule) ----------------
#ifdef OC_HAVE_FACE
    // --recognize and --enroll both need the detector, so they imply --faces.
    const bool runFaces = detectFaces || doRecognize || enrollId >= 0;

    std::unique_ptr<fd::FaceDetector> faceDetector;
    if (runFaces) {
        faceDetector = std::make_unique<fd::FaceDetector>();
        fd::FaceDetector::Options faceOptions;
        faceOptions.model_path = faceModel;
        faceOptions.score_threshold = static_cast<float>(faceScore);
        faceOptions.detection_width = faceDetectWidth;
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

    std::unique_ptr<fd::FaceRecognizer> faceRecognizer;
    fd::FaceDatabase faceDatabase;
    if (doRecognize || enrollId >= 0) {
        faceRecognizer = std::make_unique<fd::FaceRecognizer>();
        fd::FaceRecognizer::Options recOptions;
        recOptions.model_path = faceRecModel;
        if (!faceRecognizer->open(recOptions)) {
            std::cerr << "Could not load the face-recognition model '"
                      << faceRecModel << "'.\nRun "
                         "third_party/face-detection/models/fetch.sh to "
                         "download it.\n";
            camera->close();
            return 1;
        }
        std::cout << "Face recognition enabled (model: " << faceRecModel
                  << ").\n";

        if (!faceDbPath.empty() && faceDatabase.load(faceDbPath)) {
            std::cout << "Loaded " << faceDatabase.size() << " enrolment(s) from "
                      << faceDbPath << ".\n";
        } else if (doRecognize && !faceDbPath.empty()) {
            std::cout << "No face database at '" << faceDbPath
                      << "'; no one can be recognised yet.\n";
        }
    }
#else
    if (detectFaces || doRecognize || enrollId >= 0) {
        std::cerr << "--faces/--recognize/--enroll require a build with "
                     "OBJECTIVE_CONTROL_ENABLE_FACE=ON.\n";
        camera->close();
        return 1;
    }
#endif

#if defined(OC_HAVE_FACE) && defined(OC_HAVE_OPENCV)
    // Detects the faces in a frame and draws them, labelling each one with
    // the person id when the face is known. Returns the number of faces.
    auto annotateFrame = [&](cv::Mat& image) -> int {
        const fd::Image faceImage = toFaceImage(image);
        const std::vector<fd::FaceBox> faces = faceDetector->detect(faceImage);

        std::vector<std::string> labels;
        labels.reserve(faces.size());
        for (const fd::FaceBox& face : faces) {
            std::string label = "unknown";
            if (faceRecognizer) {
                fd::Embedding embedding;
                if (faceRecognizer->embed(faceImage, face, embedding)) {
                    const fd::FaceDatabase::Match match = faceDatabase.match(
                        embedding, static_cast<float>(maxDistance));
                    if (match.matched) {
                        label = "person " + std::to_string(match.person_id);
                    }
                }
            }
            labels.push_back(label);
        }

        annotateFaces(image, faces, labels);
        return static_cast<int>(faces.size());
    };
#endif

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
#if defined(OC_HAVE_FACE) && defined(OC_HAVE_OPENCV)
                        // Draw the same boxes the console path reports, so the
                        // preview window and the MJPEG stream are annotated
                        // instead of showing raw frames.
                        if (faceDetector) {
                            annotateFrame(out);
                        }
#endif
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

    // ---------------- Single/limited frame-capture path ----------------
    int received = 0;
    int facesSeen = 0;
#ifdef OC_HAVE_FACE
    int recognizedSeen = 0;
    bool enrolled = false;
#endif
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

                if (!faceRecognizer) {
                    continue;
                }

                // Identify this face against the enrolled set.
                fd::Embedding embedding;
                if (!faceRecognizer->embed(image, face, embedding)) {
                    std::cout << " -> embedding failed";
                    continue;
                }
                const fd::FaceDatabase::Match match =
                    faceDatabase.match(embedding,
                                       static_cast<float>(maxDistance));
                if (match.matched) {
                    ++recognizedSeen;
                    std::cout << " -> person " << match.person_id
                              << " (distance " << match.distance << ")";
                } else if (faceDatabase.empty()) {
                    std::cout << " -> unknown (database is empty)";
                } else {
                    std::cout << " -> unknown (distance " << match.distance
                              << ")";
                }
            }
            std::cout << '\n';

            // Enrolment takes the most confident face of the first frame
            // that contains one.
            if (enrollId >= 0 && !enrolled && !faces.empty()) {
                const fd::FaceBox* best = &faces.front();
                for (const fd::FaceBox& face : faces) {
                    if (face.score > best->score) {
                        best = &face;
                    }
                }
                if (faceDatabase.enroll(enrollId, image, *best, *faceRecognizer)) {
                    enrolled = true;
                    std::cout << "  Enrolled person " << enrollId << " ("
                              << faceDatabase.size()
                              << " sample(s) in the database).\n";
                } else {
                    std::cerr << "  Could not compute the embedding for the "
                                 "enrolment.\n";
                }
            }
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
    int exitCode = 0;
#ifdef OC_HAVE_FACE
    if (faceDetector) {
        std::cout << "Detected " << facesSeen << " face(s) in total.\n";
    }
    if (faceRecognizer) {
        std::cout << "Recognised " << recognizedSeen << " face(s) against the "
                  << "database.\n";
    }
    if (enrollId >= 0) {
        if (!enrolled) {
            std::cerr << "No face was detected, so person " << enrollId
                      << " was not enrolled.\n";
            exitCode = 1;
        } else if (!faceDatabase.save(faceDbPath)) {
            std::cerr << "Could not write the face database to '"
                      << faceDbPath << "'.\n";
            exitCode = 1;
        } else {
            std::cout << "Saved " << faceDatabase.size() << " enrolment(s) to "
                      << faceDbPath << ".\n";
        }
    }
#else
    (void)facesSeen;
#endif
    return exitCode;
}
