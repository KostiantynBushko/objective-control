#include "objective-control/camera/camera.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef OC_HAVE_OPENCV
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include "mjpeg_stream.hpp"
#endif

#ifdef OC_HAVE_FACE
#include "face-detection/face/database.hpp"
#include "face-detection/face/detector.hpp"
#include "face-detection/face/recognizer.hpp"
#include "face-detection/face/store.hpp"
#include "face-detection/json.hpp"
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
           name == "faces" || name == "recognize" || name == "help" ||
           name == "face-db-list" || name == "face-db-init" ||
           name == "auto-enroll";
}

// Every option the app understands. Anything else is a typo and is reported
// rather than silently ignored.
bool isKnownOption(const std::string& name) {
    static const char* const kOptions[] = {
        "mode",    "source", "width",       "height", "fps",
        "frames",  "seconds", "out",       "record", "stream",
        "preview", "window-size", "window-fullscreen", "help",
        "transport",
        "faces",   "face-model", "face-score", "face-detect-width",
        "recognize", "enroll", "face-db", "face-recognition-model",
        "face-max-distance", "face-same-distance", "face-db-list",
        "face-db-remove", "face-db-init", "enroll-name", "enroll-metadata",
        "auto-enroll", "face-image-size",
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
  --transport=<proto>    RTSP transport: udp (default, lowest latency) or tcp.
                         The backend falls back to TCP when the camera does
                         not accept UDP.
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
  --face-image-size=<px>
                         Size of the face image that is stored with an
                         enrolment and that the recognizer works on. A
                         face that is smaller than this is scaled up to
                         it, so that a person who stood far away still
                         gets a picture of a usable size; a bigger face
                         is left as it is. 0 keeps the size of the frame
                         (default: 320)
  --recognize            Identify every detected face against the enrolled
                         set and print the person id; with --preview or
                         --stream the person id is drawn next to the box
                         (implies --faces)
  --face-db=<target>     Face database. Either a file path or a PostgreSQL
                         connection string starting with postgres:// or
                         postgresql://. Loaded when it exists and written
                         back when --enroll is used. Defaults to the
                         $FACE_DB_URL or $DATABASE_URL environment
                         variable.
  --face-db-list         List the enrolled people and exit. Needs
                         --face-db; no camera is opened.
  --face-db-remove=<id>  Remove every sample of a person and exit. Needs
                         --face-db; no camera is opened.
  --face-db-init         Create the database schema and exit. Needs
                         --face-db; no camera is opened.
  --enroll=<id>          Enrol the most confident face of the first frame
                         that contains one, under this person id. Implies
                         --faces and requires --face-db.
  --enroll-name=<name>   Name of the person being enrolled. It is stored
                         with the sample and shown on screen when the
                         person is recognised.
  --enroll-metadata=<json>
                         Extra properties of the person, as a JSON document
                         (e.g. {"role":"admin"}). Stored as they are.
  --auto-enroll          Enrol every face that is not in the database yet.
                         A new face is given an id of its own and stored, so
                         that it is recognised from the next frame on; a face
                         that is already in the database -- whether it has a
                         name or not -- is left alone. Implies --faces and
                         --recognize and requires --face-db.
  --face-recognition-model=<path>
                         SFace model file used by --recognize/--enroll
                         (default: third_party/face-detection/models/
                          face_recognition_sface_2021dec.onnx)
  --face-max-distance=<f>
                         Largest cosine distance still counted as a match
                         (default: 0.4; lower is stricter)
  --face-same-distance=<f>
                         Distance above which a face is certainly somebody
                         new. A face between --face-max-distance and this
                         one is probably somebody who is enrolled already,
                         so --auto-enroll leaves it alone instead of
                         enrolling the same person again (default: 0.6)
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

  # Enrol into PostgreSQL instead of a file
  objective-control-app --mode=embedded --source=/dev/video0 --enroll=42 \
      --face-db=postgres://user@localhost/objective_control

  # List who is enrolled in the PostgreSQL database (no camera needed)
  objective-control-app --face-db=postgres://user@localhost/objective_control \
      --face-db-list

  # Watch the camera and enrol every new face that shows up
  objective-control-app --mode=embedded --source=/dev/video0 --preview \
      --auto-enroll --face-db=postgres://user@localhost/objective_control

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
#endif // OC_HAVE_OPENCV

#ifdef OC_HAVE_FACE
// The caption of one detected face.
struct FaceLabel {
    /// The text that is drawn next to the box.
    std::string text;

    /// True when the face belongs to an enrolled person. The box is
    /// green for a known person and amber for an unknown one.
    bool known = false;
};

// Tells the user when the store cannot keep the properties of a person.
//
// A PostgreSQL table that predates the columns and that the runtime user
// is not allowed to alter is used as it is: the name, the picture and the
// metadata of a person are then dropped on the way in, which is worth a
// word on the screen.
void warnAboutMissingProperties(const fd::FaceStore& store) {
    const std::vector<std::string> missing = store.missingPropertyColumns();
    if (missing.empty()) {
        return;
    }

    std::cerr << "Warning: the face database cannot store ";
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0) {
            std::cerr << ", ";
        }
        std::cerr << missing[i];
    }
    std::cerr << ". ";
    if (missing.size() == 1) {
        std::cerr << "This property is not kept.";
    } else {
        std::cerr << "These properties are not kept.";
    }
    std::cerr << " The table is probably missing the column(s), and the "
                 "database user is not allowed to alter it.\n";
}

// Hands out the ids of the people who are enrolled automatically.
//
// The id of a person is what tells two of them apart in the database, so
// an id that is already in use must never be handed out a second time.
// The ids that are taken are reserved here; the ones that are read back
// from the store are added to them, so that a second instance of the
// application does not pick an id that somebody else has taken in the
// meantime.
class PersonIdPool {
public:
    /// Marks an id as taken.
    void reserve(int id) {
        used_.insert(id);
        if (id >= next_) {
            next_ = id + 1;
        }
    }

    /// Hands out the lowest id that is not taken yet.
    int take() {
        while (used_.count(next_) != 0) {
            ++next_;
        }
        const int id = next_++;
        used_.insert(id);
        return id;
    }

private:
    std::set<int> used_;
    int next_ = 1;
};

#ifdef OC_HAVE_OPENCV
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

// A face cut out of a frame.
//
// The cut-out carries the box and the landmarks in the coordinates of the
// cut-out itself, so that the recognizer can be pointed at it directly.
struct FaceCrop {
    cv::Mat image;
    fd::FaceBox box;
};

// Cuts the face at @p face out of @p source and scales it.
//
// A little padding is added around the box: the edges of a face carry
// information as well. The cut-out is scaled *up* when it is smaller than
// @p size, so that a face that stood far away still gives a picture of a
// usable size. A face that is already bigger is left alone -- shrinking
// it would throw away detail that the picture can show.
//
// A size of 0 keeps the cut-out as it is.
bool cropFace(const cv::Mat& source, const fd::FaceBox& face, int size,
              FaceCrop& out) {
    const int pad = std::max(face.width, face.height) / 4;
    const int x = std::max(0, face.x - pad);
    const int y = std::max(0, face.y - pad);
    const int width = std::min(source.cols - x, face.width + 2 * pad);
    const int height = std::min(source.rows - y, face.height + 2 * pad);
    if (width <= 0 || height <= 0) {
        return false;
    }

    cv::Mat crop = source(cv::Rect(x, y, width, height));

    // Scale the cut-out up when it is smaller than the wanted size.
    double factor = 1.0;
    if (size > 0) {
        const int longest = std::max(width, height);
        if (longest < size) {
            factor = static_cast<double>(size) / static_cast<double>(longest);
        }
    }
    if (factor != 1.0) {
        const int scaledWidth =
            std::max(1, static_cast<int>(std::lround(width * factor)));
        const int scaledHeight =
            std::max(1, static_cast<int>(std::lround(height * factor)));
        cv::Mat scaled;
        // The cubic filter is a low-pass one, so the picture does not
        // get the hard edges that a bilinear stretch would give it.
        cv::resize(crop, scaled, cv::Size(scaledWidth, scaledHeight), 0.0,
                   0.0, cv::INTER_CUBIC);
        crop = scaled;
    }

    // The box and the landmarks move with the cut-out.
    fd::FaceBox moved = face;
    moved.x = static_cast<int>(std::lround((face.x - x) * factor));
    moved.y = static_cast<int>(std::lround((face.y - y) * factor));
    moved.width = static_cast<int>(std::lround(face.width * factor));
    moved.height = static_cast<int>(std::lround(face.height * factor));
    const float originX = static_cast<float>(x);
    const float originY = static_cast<float>(y);
    const float scale = static_cast<float>(factor);
    for (std::size_t lm = 0; lm < fd::kLandmarkCount; ++lm) {
        moved.landmarks[lm][0] = (face.landmarks[lm][0] - originX) * scale;
        moved.landmarks[lm][1] = (face.landmarks[lm][1] - originY) * scale;
    }

    out.image = crop;
    out.box = moved;
    return true;
}

// Encodes the detected face as a JPEG, so that the picture of the person
// is stored with the enrolment. Returns an empty vector when the face
// could not be encoded.
std::vector<std::uint8_t> encodeFace(const cv::Mat& source,
                                      const fd::FaceBox& face, int size) {
    FaceCrop crop;
    if (!cropFace(source, face, size, crop)) {
        return {};
    }

    std::vector<uchar> buffer;
    if (!cv::imencode(".jpg", crop.image, buffer, {cv::IMWRITE_JPEG_QUALITY, 95})) {
        return {};
    }
    return std::vector<std::uint8_t>(buffer.begin(), buffer.end());
}

// Computes the embedding of the face at @p face.
//
// The face is cut out of the frame and scaled first, so that the
// embedding is computed from the very image that is stored with the
// enrolment: what the editor shows of a person is what was recognised.
bool embedFace(const cv::Mat& source, const fd::FaceBox& face, int size,
               const fd::FaceRecognizer& recognizer,
               fd::Embedding& embedding) {
    FaceCrop crop;
    if (!cropFace(source, face, size, crop)) {
        return false;
    }
    return recognizer.embed(toFaceImage(crop.image), crop.box, embedding);
}

// Draws a box and a caption for every detected face. A recognised person
// gets a green box, an unknown face an amber one, so the overlay can be
// read at a glance.
void annotateFaces(cv::Mat& image, const std::vector<fd::FaceBox>& faces,
                   const std::vector<FaceLabel>& labels) {
    // Scale the stroke and the caption with the frame so the overlay stays
    // readable on both a 320x240 sub-stream and a 4K one.
    const int thickness = std::max(1, image.cols / 400);
    const double fontScale =
        std::max(0.4, static_cast<double>(image.cols) / 1600.0);

    for (std::size_t i = 0; i < faces.size(); ++i) {
        const fd::FaceBox& face = faces[i];
        const FaceLabel label =
            (i < labels.size()) ? labels[i] : FaceLabel();
        const std::string& text = label.text;
        const bool known = label.known;

        const cv::Scalar color =
            known ? cv::Scalar(0, 220, 0) : cv::Scalar(0, 165, 255);
        cv::rectangle(image,
                      cv::Rect(face.x, face.y, face.width, face.height),
                      color, thickness);

        if (text.empty()) {
            continue;
        }

        // Caption above the box; flip it below when there is no room.
        int baseline = 0;
        const cv::Size textSize =
            cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, fontScale,
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
        cv::putText(image, text, cv::Point(textX + 4, textY - 4),
                    cv::FONT_HERSHEY_SIMPLEX, fontScale, cv::Scalar(0, 0, 0),
                    thickness, cv::LINE_AA);
    }
}
#endif // OC_HAVE_OPENCV
#endif // OC_HAVE_FACE

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
    // The size of the face image that is stored with an enrolment and
    // that the recognizer works on. A face that is smaller than this is
    // scaled up to it, so that a person who stood far away still gets a
    // picture of a usable size. A bigger face is left as it is.
    const std::string faceImageSizeStr =
        arg(argc, argv, "face-image-size", "320");
    int faceImageSize = 0;
    try {
        faceImageSize = std::stoi(faceImageSizeStr);
    } catch (const std::exception&) {
        faceImageSize = -1;
    }
    if (faceImageSize < 0) {
        std::cerr << "Invalid --face-image-size value '" << faceImageSizeStr
                  << "' (expected a number of pixels, or 0 to keep the size "
                     "of the frame).\n";
        return 2;
    }
    const bool doRecognize = flag(argc, argv, "recognize");
    const bool autoEnroll = flag(argc, argv, "auto-enroll");
    const std::string enrollStr = arg(argc, argv, "enroll", "");
    const std::string enrollName = arg(argc, argv, "enroll-name", "");
    const std::string enrollMetadata = arg(argc, argv, "enroll-metadata", "");
    const std::string faceDbPath = arg(argc, argv, "face-db", "");
    const bool faceDbList = flag(argc, argv, "face-db-list");
    const bool faceDbInit = flag(argc, argv, "face-db-init");
    const std::string faceDbRemoveStr = arg(argc, argv, "face-db-remove", "");
    const std::string faceRecModel = arg(argc, argv, "face-recognition-model",
                                         "third_party/face-detection/models/"
                                         "face_recognition_sface_2021dec.onnx");
    const double maxDistance =
        std::atof(arg(argc, argv, "face-max-distance", "0.4").c_str());

    // The distance above which a face is certainly somebody new. Between
    // --face-max-distance and this one, a face is very likely somebody who
    // is enrolled already -- the camera sees a face differently every time
    // (another pose, another distance, a little less light) -- and it is
    // not enrolled a second time.
    double sameDistance =
        std::atof(arg(argc, argv, "face-same-distance", "0.6").c_str());
    if (sameDistance <= maxDistance) {
        std::cerr << "Ignoring --face-same-distance=" << sameDistance
                  << ": it has to be larger than --face-max-distance ("
                  << maxDistance << ").\n";
        sameDistance = maxDistance;
    }

    // The face database lives either in a file or in a PostgreSQL table.
    // The target says which: a path, or a postgres:// connection string.
    // An environment variable provides the default, so that a deployment
    // does not have to repeat it on every command line.
    std::string faceDbTarget = faceDbPath;
    if (faceDbTarget.empty()) {
        const char* faceDbUrl = std::getenv("FACE_DB_URL");
        const char* databaseUrl = std::getenv("DATABASE_URL");
        if (faceDbUrl != nullptr && *faceDbUrl != '\0') {
            faceDbTarget = faceDbUrl;
        } else if (databaseUrl != nullptr && *databaseUrl != '\0') {
            faceDbTarget = databaseUrl;
        }
    }

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
        if (faceDbTarget.empty()) {
            std::cerr << "--enroll requires --face-db=<target> to store the "
                         "enrolment.\n";
            return 2;
        }
    }

    // --auto-enroll enrols the faces that are not in the database yet. It
    // needs a place to put them, and it cannot be combined with --enroll:
    // one of the two names the person to enrol, the other enrols whoever
    // shows up.
    if (autoEnroll) {
        if (faceDbTarget.empty()) {
            std::cerr << "--auto-enroll requires --face-db=<target> to store "
                         "the faces it enrols.\n";
            return 2;
        }
        if (enrollId >= 0) {
            std::cerr << "--auto-enroll and --enroll cannot be used together: "
                         "--enroll names the person to enrol, --auto-enroll "
                         "enrols every face that is not in the database yet.\n";
            return 2;
        }
    }

#ifdef OC_HAVE_FACE
    // The metadata is stored in a JSON column, so it has to be a JSON
    // document. It is checked here, because the error of the server is a
    // lot harder to make sense of.
    if (!enrollMetadata.empty() && !fd::isValidJson(enrollMetadata)) {
        std::cerr << "Invalid --enroll-metadata value '" << enrollMetadata
                  << "': not a JSON document.\n";
        return 2;
    }
#endif

    // --face-db-remove=<id> names the person to remove.
    int faceDbRemoveId = -1;
    if (!faceDbRemoveStr.empty()) {
        try {
            faceDbRemoveId = std::stoi(faceDbRemoveStr);
        } catch (const std::exception&) {
            faceDbRemoveId = -1;
        }
        if (faceDbRemoveId < 0) {
            std::cerr << "Invalid --face-db-remove value '" << faceDbRemoveStr
                      << "' (expected a non-negative integer).\n";
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

    // RTSP transport. Empty means "udp, fall back to tcp", which is what
    // the backend does when the camera does not accept UDP.
    const std::string transport = arg(argc, argv, "transport", "");
    if (!transport.empty() && transport != "udp" && transport != "tcp") {
        std::cerr << "Invalid --transport value '" << transport
                  << "' (expected 'udp' or 'tcp').\n";
        return 2;
    }
    config.transport = transport;

    // A preview and an MJPEG stream are live: the consumer is usually
    // slower than the camera (decoding, drawing, JPEG encoding all take
    // time), so the backend has to hand out the newest frame rather than
    // the oldest one it has buffered. A recording keeps every frame, so it
    // stays on the lossless path.
    config.live = preview || streamPort > 0;

    // ---------------- Face database administration ----------------
    // Listing, removing and initialising the database do not need a camera,
    // so they are handled before one is opened.
    const bool dbAdmin =
        faceDbList || faceDbInit || faceDbRemoveId >= 0;
    if (dbAdmin) {
#ifdef OC_HAVE_FACE
        if (faceDbTarget.empty()) {
            std::cerr << "--face-db-list, --face-db-init and "
                         "--face-db-remove need --face-db=<target>.\n";
            return 2;
        }

        std::string error;
        auto store = fd::makeFaceStore(faceDbTarget, error);
        if (!store) {
            std::cerr << error << '\n';
            return 1;
        }
        if (!store->open(faceDbTarget, error)) {
            std::cerr << "Could not open the face database '" << faceDbTarget
                      << "': " << error << '\n';
            return 1;
        }
        warnAboutMissingProperties(*store);

        if (faceDbList) {
            std::vector<fd::PersonSummary> people;
            if (!store->list(people, error)) {
                std::cerr << "Could not list the face database: " << error
                          << '\n';
                return 1;
            }
            if (people.empty()) {
                std::cout << "Nobody is enrolled in " << faceDbTarget << ".\n";
            }
            for (const fd::PersonSummary& person : people) {
                std::cout << "  person " << person.person_id << ": "
                          << person.samples << " sample(s)";
                if (!person.user_name.empty()) {
                    std::cout << " (" << person.user_name << ")";
                }
                std::cout << '\n';
            }
            return 0;
        }

        if (faceDbRemoveId >= 0) {
            const int removed = store->remove(faceDbRemoveId, error);
            if (removed < 0) {
                std::cerr << "Could not remove person " << faceDbRemoveId
                          << ": " << error << '\n';
                return 1;
            }
            std::cout << "Removed " << removed << " sample(s) of person "
                      << faceDbRemoveId << ".\n";
            return 0;
        }

        // --face-db-init: a PostgreSQL store creates its schema here; a
        // file store gets an empty database file.
        if (fd::isPostgresTarget(faceDbTarget)) {
            // The schema is prepared again, and this time every problem
            // is reported: the user asked for it to be set up.
            if (!store->prepareSchema(error)) {
                std::cerr << "Could not prepare the face database schema: "
                          << error << '\n';
                return 1;
            }
            std::cout << "The face database schema in " << faceDbTarget
                      << " is ready.\n";
        } else {
            const std::vector<fd::FaceEntry> none;
            if (!store->save(none, error)) {
                std::cerr << "Could not create the face database: " << error
                          << '\n';
                return 1;
            }
            std::cout << "Created the face database " << faceDbTarget
                      << ".\n";
        }
        return 0;
#else
        std::cerr << "--face-db-list, --face-db-init and --face-db-remove "
                     "require a build with OBJECTIVE_CONTROL_ENABLE_FACE=ON.\n";
        return 1;
#endif
    }

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
    // --recognize, --enroll and --auto-enroll all need the detector, so
    // they imply --faces.
    const bool runFaces =
        detectFaces || doRecognize || enrollId >= 0 || autoEnroll;

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
    std::unique_ptr<fd::FaceStore> faceStore;
    if (doRecognize || enrollId >= 0 || autoEnroll) {
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

        // The store holds the samples; the database keeps them in memory
        // so that the matching does not have to hit the database for every
        // frame.
        if (!faceDbTarget.empty()) {
            std::string error;
            faceStore = fd::makeFaceStore(faceDbTarget, error);
            if (!faceStore) {
                std::cerr << error << '\n';
                camera->close();
                return 1;
            }
            if (!faceStore->open(faceDbTarget, error)) {
                std::cerr << "Could not open the face database '"
                          << faceDbTarget << "': " << error << '\n';
                camera->close();
                return 1;
            }
            warnAboutMissingProperties(*faceStore);

            std::vector<fd::FaceEntry> entries;
            if (faceStore->load(entries, error)) {
                for (const fd::FaceEntry& entry : entries) {
                    // The whole entry is added, so that the name, the
                    // picture and the metadata of the person survive the
                    // reload.
                    faceDatabase.add(entry);
                }
                std::cout << "Loaded " << entries.size() << " enrolment(s) from "
                          << faceDbTarget << ".\n";
            } else {
                std::cout << "No enrolments in '" << faceDbTarget
                          << "' yet (" << error << ").\n";
            }
        }
    }
#else
    if (detectFaces || doRecognize || enrollId >= 0 || autoEnroll) {
        std::cerr << "--faces/--recognize/--enroll/--auto-enroll require a "
                     "build with OBJECTIVE_CONTROL_ENABLE_FACE=ON.\n";
        camera->close();
        return 1;
    }
#endif

#ifdef OC_HAVE_FACE
    // The ids of the people who are already in the database. A person who
    // is enrolled automatically gets an id of their own, and that id must
    // not be one of these.
    PersonIdPool personIds;
    for (const fd::FaceEntry& entry : faceDatabase.entries()) {
        personIds.reserve(entry.person_id);
    }

    // Hands out an id that is not in use yet.
    //
    // The ids are read back from the store before one is handed out, so
    // that a second instance of the application does not pick an id that
    // somebody else has taken in the meantime.
    auto allocatePersonId = [&]() -> int {
        if (faceStore) {
            std::vector<fd::PersonSummary> people;
            std::string error;
            if (faceStore->list(people, error)) {
                for (const fd::PersonSummary& person : people) {
                    personIds.reserve(person.person_id);
                }
            }
        }
        return personIds.take();
    };

    // What was found out about one face of a frame.
    struct FaceResult {
        fd::FaceBox box;

        /// True when the face is in the database.
        bool known = false;

        /// True when the face is not in the database, but is close enough
        /// to one of its samples to be very likely the same person.
        bool probable = false;

        /// The person the face belongs to.
        int person_id = -1;

        /// The distance to the closest sample of that person.
        float distance = 1.0f;

        /// True when the embedding of the face could be computed.
        bool embedded = false;

        /// True when this run enrolled the face.
        bool enrolled = false;

        /// The name of the person, when the sample carries one.
        std::string user_name;
    };

    // Detects the faces in a frame and works out who each of them is.
    //
    // A face that is not in the database yet is enrolled when --auto-enroll
    // is on: it is given an id of its own and stored, so that it is
    // recognised from the next frame on like anybody else. A face that is
    // in the database already -- whether its person has a name or not --
    // is left alone.
    //
    // @p pictureOf makes the picture of a face. It is empty when the build
    // cannot make one.
    //
    // @p embedOf computes the embedding of a face. It is empty when the
    // build cannot make one, and the embedding is then computed from the
    // whole frame instead.
    auto analyseFaces =
        [&](const fd::Image& image,
            const std::function<std::vector<std::uint8_t>(const fd::FaceBox&)>&
                pictureOf,
            const std::function<bool(const fd::FaceBox&, fd::Embedding&)>&
                embedOf) -> std::vector<FaceResult> {
        std::vector<FaceResult> results;
        if (!faceDetector) {
            return results;
        }

        const std::vector<fd::FaceBox> faces = faceDetector->detect(image);
        results.reserve(faces.size());

        for (const fd::FaceBox& face : faces) {
            FaceResult result;
            result.box = face;

            if (!faceRecognizer) {
                results.push_back(result);
                continue;
            }

            fd::Embedding embedding;
            const bool embedded = embedOf ? embedOf(face, embedding)
                                          : faceRecognizer->embed(image, face,
                                                                  embedding);
            if (!embedded) {
                results.push_back(result);
                continue;
            }
            result.embedded = true;

            const fd::FaceDatabase::Match match = faceDatabase.match(
                embedding, static_cast<float>(maxDistance));
            result.distance = match.distance;

            if (match.matched) {
                // The face is in the database already. Nothing is stored
                // for it, whatever its person is called.
                result.known = true;
                result.person_id = match.person_id;
                result.user_name = match.user_name;
                results.push_back(result);
                continue;
            }

            // The face is not in the database -- but it is close enough
            // to one of its samples to be very likely the same person. A
            // second enrolment of somebody who is enrolled already would
            // fill the database with copies of them, so such a face is
            // reported and left alone.
            if (autoEnroll && faceStore && match.person_id >= 0 &&
                match.distance <= sameDistance) {
                result.probable = true;
                result.person_id = match.person_id;
                result.user_name = match.user_name;
                results.push_back(result);
                continue;
            }

            // The face is not in the database.
            if (autoEnroll && faceStore) {
                // The properties of the new person: the name is left empty
                // on purpose, so that it can be filled in later with the
                // face database editor. The metadata says where the person
                // came from.
                fd::EnrolmentInfo info;
                info.metadata = "{\"auto_enrolled\": true}";
                if (pictureOf) {
                    info.photo = pictureOf(face);
                }

                const int personId = allocatePersonId();
                fd::FaceEntry entry;
                if (faceDatabase.enroll(personId, embedding, info, &entry)) {
                    std::string error;
                    if (faceStore->add(entry, error)) {
                        result.known = true;
                        result.person_id = personId;
                        result.enrolled = true;
                        result.distance = 0.0f;
                        std::cout << "  Enrolled a new face as person "
                                  << personId << ".\n";
                    } else {
                        // The face is not in the database after all, so it
                        // is taken out of it again: it is enrolled by the
                        // next frame that shows it.
                        faceDatabase.remove(personId);
                        std::cerr << "  Could not store the new person "
                                  << personId << " in '" << faceDbTarget
                                  << "': " << error << '\n';
                    }
                }
            }

            results.push_back(result);
        }
        return results;
    };
#endif

#if defined(OC_HAVE_FACE) && defined(OC_HAVE_OPENCV)
    // Detects the faces in a frame and draws them, labelling each one with
    // the person id when the face is known. Returns the number of faces.
    auto annotateFrame = [&](cv::Mat& image) -> int {
        const fd::Image faceImage = toFaceImage(image);
        const std::vector<FaceResult> results = analyseFaces(
            faceImage,
            [&](const fd::FaceBox& box) {
                return encodeFace(image, box, faceImageSize);
            },
            [&](const fd::FaceBox& box, fd::Embedding& embedding) {
                if (!faceRecognizer) {
                    return false;
                }
                return embedFace(image, box, faceImageSize, *faceRecognizer,
                                 embedding);
            });

        std::vector<fd::FaceBox> boxes;
        std::vector<FaceLabel> labels;
        boxes.reserve(results.size());
        labels.reserve(results.size());
        for (const FaceResult& result : results) {
            boxes.push_back(result.box);

            FaceLabel label;
            label.known = result.known;
            if (result.known) {
                // The name of the person is shown when there is one; the
                // id is the fallback.
                label.text = result.user_name.empty()
                                 ? "person " + std::to_string(result.person_id)
                                 : result.user_name;
            } else if (result.probable) {
                // Somebody who is enrolled already, most likely. The name
                // carries a question mark and the box stays amber, because
                // the match is not certain.
                const std::string who =
                    result.user_name.empty()
                        ? "person " + std::to_string(result.person_id)
                        : result.user_name;
                label.text = who + "?";
            } else {
                label.text = "unknown";
            }
            labels.push_back(label);
        }

        annotateFaces(image, boxes, labels);
        return static_cast<int>(boxes.size());
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
    int probableSeen = 0;
    int autoEnrolledSeen = 0;
    bool enrolled = false;
    bool enrollmentStored = true;
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

            // The picture of the face is stored with an enrolment, when the
            // build can make one.
            std::function<std::vector<std::uint8_t>(const fd::FaceBox&)>
                pictureOf;
            // The embedding of the face is computed from the picture that
            // is stored, so that the two always agree.
            std::function<bool(const fd::FaceBox&, fd::Embedding&)> embedOf;
#ifdef OC_HAVE_OPENCV
            pictureOf = [&](const fd::FaceBox& box) {
                cv::Mat view;
                if (!toMat(*frame, view)) {
                    return std::vector<std::uint8_t>();
                }
                return encodeFace(view, box, faceImageSize);
            };
            embedOf = [&](const fd::FaceBox& box, fd::Embedding& embedding) {
                cv::Mat view;
                if (!toMat(*frame, view) || !faceRecognizer) {
                    return false;
                }
                return embedFace(view, box, faceImageSize, *faceRecognizer,
                                 embedding);
            };
#endif

            const std::vector<FaceResult> results =
                analyseFaces(image, pictureOf, embedOf);

            facesSeen += static_cast<int>(results.size());
            std::cout << "  faces: " << results.size();
            for (const FaceResult& result : results) {
                const fd::FaceBox& face = result.box;
                std::cout << " (" << face.x << "," << face.y << " "
                          << face.width << "x" << face.height << " score="
                          << face.score << ")";

                if (!faceRecognizer) {
                    continue;
                }
                if (!result.embedded) {
                    std::cout << " -> embedding failed";
                    continue;
                }
                if (result.known) {
                    ++recognizedSeen;
                    if (result.enrolled) {
                        ++autoEnrolledSeen;
                    }
                    if (result.user_name.empty()) {
                        std::cout << " -> person " << result.person_id
                                  << " (distance " << result.distance << ")";
                    } else {
                        std::cout << " -> " << result.user_name << " (person "
                                  << result.person_id << ", distance "
                                  << result.distance << ")";
                    }
                } else if (result.probable) {
                    ++probableSeen;
                    // Somebody who is enrolled already, most likely: the
                    // face was not enrolled a second time.
                    if (result.user_name.empty()) {
                        std::cout << " -> probably person " << result.person_id
                                  << " (distance " << result.distance << ")";
                    } else {
                        std::cout << " -> probably " << result.user_name
                                  << " (person " << result.person_id
                                  << ", distance " << result.distance << ")";
                    }
                } else if (faceDatabase.empty()) {
                    std::cout << " -> unknown (database is empty)";
                } else {
                    std::cout << " -> unknown (distance " << result.distance
                              << ")";
                }
            }
            std::cout << '\n';

            // Enrolment takes the most confident face of the first frame
            // that contains one.
            if (enrollId >= 0 && !enrolled) {
                const FaceResult* best = nullptr;
                for (const FaceResult& result : results) {
                    if (best == nullptr ||
                        result.box.score > best->box.score) {
                        best = &result;
                    }
                }

                if (best != nullptr) {
                    const fd::FaceBox& box = best->box;

                    // The properties of the person: the name that is shown
                    // when the person is recognised, the picture of the face
                    // and the metadata.
                    fd::EnrolmentInfo info;
                    info.user_name = enrollName;
                    info.metadata = enrollMetadata;
#ifdef OC_HAVE_OPENCV
                    {
                        // The picture of the face is stored with the sample,
                        // so that the person can be recognised by eye later on.
                        cv::Mat view;
                        if (toMat(*frame, view)) {
                            info.photo = encodeFace(view, box, faceImageSize);
                        }
                    }
#endif

                    // A store that cannot keep the properties drops them on
                    // the way in, so the messages below have to say so.
                    const bool storeKeepsProperties =
                        faceStore == nullptr ||
                        faceStore->missingPropertyColumns().empty();
                    const auto noteDropped = [&](const char* what) {
                        if (storeKeepsProperties) {
                            return;
                        }
                        std::cout << " (not stored: the face database cannot "
                                     "keep " << what << ")";
                    };

                    fd::FaceEntry entry;
                    if (faceDatabase.enroll(enrollId, image, box,
                                            *faceRecognizer, info, &entry)) {
                        enrolled = true;
                        std::cout << "  Enrolled person " << enrollId << " ("
                                  << faceDatabase.size()
                                  << " sample(s) in the database).\n";
                        if (!info.user_name.empty()) {
                            std::cout << "  The name of the person is \""
                                      << info.user_name << "\"";
                            noteDropped("it");
                            std::cout << ".\n";
                        }
                        if (!info.photo.empty()) {
                            std::cout << "  The picture of the face is "
                                      << info.photo.size() << " bytes";
                            noteDropped("it");
                            std::cout << ".\n";
                        }
                        if (!info.metadata.empty()) {
                            std::cout << "  The metadata of the person is "
                                      << info.metadata;
                            noteDropped("it");
                            std::cout << ".\n";
                        }

                        // The store appends the new sample on its own, so
                        // enrolling one person never rewrites the samples of
                        // the others.
                        if (faceStore) {
                            std::string error;
                            if (!faceStore->add(entry, error)) {
                                std::cerr
                                    << "  Could not store the enrolment in '"
                                    << faceDbTarget << "': " << error << '\n';
                                enrollmentStored = false;
                            } else {
                                std::cout << "  Stored the enrolment in "
                                          << faceDbTarget << ".\n";
                            }
                        }
                    } else {
                        std::cerr << "  Could not compute the embedding for "
                                     "the enrolment.\n";
                    }
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
    if (autoEnroll) {
        std::cout << "Enrolled " << autoEnrolledSeen << " new face(s) in "
                  << faceDbTarget << ".\n";
    }
    if (probableSeen > 0) {
        std::cout << "Probably " << probableSeen
                  << " face(s) belonged to somebody who is enrolled already; "
                     "they were not enrolled again.\n";
    }
    if (enrollId >= 0) {
        if (!enrolled) {
            std::cerr << "No face was detected, so person " << enrollId
                      << " was not enrolled.\n";
            exitCode = 1;
        } else if (!enrollmentStored) {
            std::cerr << "Person " << enrollId
                      << " was enrolled in memory but could not be stored in '"
                      << faceDbTarget << "'.\n";
            exitCode = 1;
        } else {
            std::cout << "The enrolment of person " << enrollId
                      << " is stored in " << faceDbTarget << ".\n";
        }
    }
#else
    (void)facesSeen;
#endif
    return exitCode;
}
