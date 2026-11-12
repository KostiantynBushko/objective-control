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
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
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
#include "face-detection/face/postgres_store.hpp"
#include "face-detection/face/recognizer.hpp"
#include "face-detection/face/store.hpp"
#include "face-detection/json.hpp"
#include "objective-control/face/auto_enroller.hpp"
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

// Parses a whole number, e.g. "3" or "64". Returns false when the text is
// not a number or holds something else besides the digits.
bool parseInteger(const std::string& text, int& out) {
    std::size_t used = 0;
    try {
        out = std::stoi(text, &used);
    } catch (const std::exception&) {
        return false;
    }
    return used == text.size();
}

// Parses a number, e.g. "0.35" or "20". Returns false when the text is not
// a number.
bool parseNumber(const std::string& text, double& out) {
    std::size_t used = 0;
    try {
        out = std::stod(text, &used);
    } catch (const std::exception&) {
        return false;
    }
    return used == text.size() && std::isfinite(out);
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
           name == "face-db-reembed" || name == "face-db-duplicates" ||
           name == "auto-enroll" || name == "auto-enroll-strict" ||
           name == "auto-enroll-attach" ||
           name == "auto-enroll-verbose";
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
        "face-db-remove", "face-db-init", "face-db-reembed",
        "face-db-table", "face-db-merge", "face-db-duplicates",
        "face-db-duplicates-distance", "face-match-samples",
        "enroll-name", "enroll-metadata",
        "auto-enroll", "face-image-size",
        "auto-enroll-mode", "auto-enroll-samples", "auto-enroll-window",
        "auto-enroll-timeout", "auto-enroll-consistency",
        "auto-enroll-diversity", "auto-enroll-min-size",
        "auto-enroll-strict", "auto-enroll-attach", "auto-enroll-verbose",
        "auto-enroll-attach-max",
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
  --face-db-reembed      Recompute the vector of every stored face from the
                         picture that is stored with it and write the result
                         back. A face that was enrolled with the landmarks
                         of a detection in the frame holds a vector that does
                         not describe its own picture; this repairs that.
                         Needs --face-db; no camera is opened.
  --face-db-table=<name> Table of the PostgreSQL face database. It is
                         face_enrollments by default; naming another one is
                         what makes it possible to work on a copy of the
                         database without touching the real one.
  --face-db-merge=<from>:<to>
                         Move the samples of one person to another one, so
                         that a face that was enrolled twice is one person
                         again. The "from" part may hold several ids,
                         separated by commas (9,10,13:1). The rows keep
                         their identity and the time they were enrolled.
                         Needs --face-db; no camera is opened.
  --enroll=<id>          Enrol the most confident face of the first frame
                         that contains one, under this person id. Implies
                         --faces and requires --face-db.
  --enroll-name=<name>   Name of the person being enrolled. It is stored
                         with the sample and shown on screen when the
                         person is recognised.
  --enroll-metadata=<json>
                         Extra properties of the person, as a JSON document
                         (e.g. {"role":"admin"}). Stored as they are.
  --auto-enroll          Enrol the faces that are not in the database yet.
                         A face is watched in memory first: the application
                         keeps the pictures and the embeddings of the face
                         and writes the person to the database only when
                         the face has been recognised with high confidence.
                         The person is stored with every view that was
                         collected. Implies --faces and --recognize and
                         requires --face-db.
  --auto-enroll-mode=<m> deferred (default) or immediate. "immediate" is the
                         old behaviour: every face that is not in the
                         database is enrolled and stored at once.
  --auto-enroll-samples=<n>
                         Number of views to collect before the person is
                         stored (default: 3)
  --auto-enroll-window=<s>
                         Seconds to collect the views of a face. When the
                         window is over, a face that is confident is stored
                         with the views it has (default: 20)
  --auto-enroll-timeout=<s>
                         Seconds a face may be out of sight before it is
                         forgotten (default: 10)
  --auto-enroll-consistency=<f>
                         Largest cosine distance between two views of the
                         same face. A face whose views are further apart
                         than this is not the same face (default: 0.35)
  --auto-enroll-diversity=<f>
                         Smallest cosine distance between two views of the
                         same face. Two views that are closer than this are
                         the same picture and only one of them is kept
                         (default: 0.08)
  --auto-enroll-min-size=<px>
                         Smallest face that is worth keeping, in pixels
                         (default: 64)
  --auto-enroll-strict   Throw a face away instead of storing it with the
                         views it has when the window is over
  --auto-enroll-attach-max=<n>
                         How many samples a person may hold before the
                         faces that look like it are no longer added to it.
                         0 means no limit (default: 20)
  --auto-enroll-attach   Add the collected views to a person that is
                         already in the database instead of creating a new
                         one. The views are only added while the person
                         holds fewer than --auto-enroll-samples samples.
  --auto-enroll-verbose  Print every view that is accepted and rejected,
                         with its distance
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
  --face-match-samples=<n>
                         How many samples of a person are averaged when a
                         face is matched against the people of the
                         database. One is the distance to the closest
                         sample; more than one is steadier, and is what
                         makes a person who was enrolled several times
                         recognisable (default: 3)
  --face-db-duplicates
                         Report the people of the database that look like
                         one person, so that they can be merged with
                         --face-db-merge. Needs --face-db; no camera is
                         opened.
  --face-db-duplicates-distance=<f>
                         The distance below which two people are reported
                         as probably one person (default: 0.6)
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
      --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control

  # List who is enrolled in the PostgreSQL database (no camera needed)
  objective-control-app --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control \
      --face-db-list

  # Watch the camera and enrol every new face that shows up
  objective-control-app --mode=embedded --source=/dev/video0 --preview \
      --auto-enroll --face-db=postgres://objective_control:objective_control@127.0.0.1:5432/objective_control

  # Store a face as soon as it is seen, the way the earlier versions did
  objective-control-app --mode=embedded --source=/dev/video0 --preview \
      --auto-enroll --auto-enroll-mode=immediate --face-db=faces.db

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
    /// How the face is shown. The colour of the box says it.
    enum class State {
        Unknown,  ///< The face is not in the database.
        Known,    ///< The face belongs to an enrolled person.
        Probable, ///< The face is probably somebody who is enrolled already.
        Tracking, ///< The face is being watched by the tracker.
    };

    /// The text that is drawn next to the box.
    std::string text;

    /// The second line of the caption, when there is one.
    std::string detail;

    /// What the state of the face is.
    State state = State::Unknown;
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

// The size of the cut-out of a face.
//
// The cut-out has a padding around the box, so that the edges of the face
// are in the picture as well. The size is measured before the cut-out is
// scaled up for the recognizer: what the quality gate is about is how
// many pixels the face had in the frame.
int cropWidth(const fd::FaceBox& face) {
    const int pad = std::max(face.width, face.height) / 4;
    return face.width + 2 * pad;
}

int cropHeight(const fd::FaceBox& face) {
    const int pad = std::max(face.width, face.height) / 4;
    return face.height + 2 * pad;
}

// Why the tracker did what it did, in a few words.
std::string trackerReason(oc::AutoEnroller::Decision::Reason reason) {
    switch (reason) {
    case oc::AutoEnroller::Decision::Reason::None:
        return "no reason";
    case oc::AutoEnroller::Decision::Reason::InvalidEmbedding:
        return "the embedding could not be used";
    case oc::AutoEnroller::Decision::Reason::TooSmall:
        return "the face is too small";
    case oc::AutoEnroller::Decision::Reason::ScoreTooLow:
        return "the detector was not sure enough";
    case oc::AutoEnroller::Decision::Reason::Duplicate:
        return "the same picture as another view";
    case oc::AutoEnroller::Decision::Reason::Inconsistent:
        return "not the same person";
    case oc::AutoEnroller::Decision::Reason::TooManyCandidates:
        return "too many faces are being watched at once";
    case oc::AutoEnroller::Decision::Reason::EnoughViews:
        return "the face has as many views as it was asked for";
    case oc::AutoEnroller::Decision::Reason::WindowOver:
        return "the collection window is over";
    case oc::AutoEnroller::Decision::Reason::TimedOut:
        return "the face was out of sight for too long";
    }
    return "unknown reason";
}

// The time the tracker works with, in microseconds.
//
// The window and the timeout of the tracker say how long the application
// has been watching a face, so they are measured with the clock of the
// application itself. The timestamp of a frame is the capture time of the
// picture, which not every backend fills in.
std::uint64_t nowMicroseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// A distance as it is shown to the user: three digits are enough for a
// cosine distance and they keep the output readable.
std::string formatDistance(float value) {
    std::ostringstream out;
    out << std::setprecision(3) << value;
    return out.str();
}

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

// Creates the store of @p target and opens it.
//
// A PostgreSQL store reads the name of its table from its options, so the
// table of --face-db-table is put in place before the store is opened. A
// file store has no table.
std::unique_ptr<fd::FaceStore> openFaceStore(const std::string& target,
                                               const std::string& table,
                                               std::string& error) {
    auto store = fd::makeFaceStore(target, error);
    if (!store) {
        return nullptr;
    }
    if (!table.empty()) {
        if (auto* postgres =
                dynamic_cast<fd::PostgresFaceStore*>(store.get())) {
            fd::PostgresFaceStore::Options options = postgres->options();
            options.table = table;
            postgres->setOptions(options);
        }
    }
    if (!store->open(target, error)) {
        return nullptr;
    }
    return store;
}

// True when the JSON document holds the given field.
//
// The metadata of a person is a JSON document. The library checks that a
// document is a valid one but does not parse it, so the field is looked
// up textually -- which is safe for the flat documents that the metadata
// of a person holds.
bool hasJsonField(const std::string& document, const std::string& key) {
    return document.find("\"" + key + "\"") != std::string::npos;
}

// Adds a field to a JSON object.
//
// The metadata of a person is a JSON document. The library checks that a
// document is a valid one but does not parse it, so the field is added
// textually -- which is safe for the flat documents that the metadata of
// a person holds.
std::string addJsonField(const std::string& document, const std::string& key,
                          const std::string& value) {
    const std::string field = "\"" + key + "\": " + value;
    if (document.empty() || document == "{}") {
        return "{" + field + "}";
    }
    const std::size_t end = document.rfind('}');
    if (end == std::string::npos) {
        return document;
    }
    const std::string head = document.substr(0, end);
    if (!head.empty() && head.back() != '{') {
        return head + ", " + field + document.substr(end);
    }
    return head + field + document.substr(end);
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

// The face that lies on the face that was cut out.
//
// A cut-out of a crowded scene can hold more than one face, so the one
// whose centre lies closest to the centre of the face that was cut out is
// the one that belongs to it. Returns false when the cut-out holds no face
// at all.
bool faceInCutOut(const fd::Image& image, const fd::FaceBox& box,
                  const fd::FaceDetector& detector, fd::FaceBox& out) {
    const std::vector<fd::FaceBox> faces = detector.detect(image);
    if (faces.empty()) {
        return false;
    }

    const int centreX = box.x + box.width / 2;
    const int centreY = box.y + box.height / 2;

    const fd::FaceBox* best = nullptr;
    long bestDistance = 0;
    for (const fd::FaceBox& face : faces) {
        const long dx = static_cast<long>(face.x + face.width / 2) - centreX;
        const long dy = static_cast<long>(face.y + face.height / 2) - centreY;
        const long distance = dx * dx + dy * dy;
        if (best == nullptr || distance < bestDistance) {
            best = &face;
            bestDistance = distance;
        }
    }
    if (best == nullptr) {
        return false;
    }
    out = *best;
    return true;
}

// Computes the embedding of the face at @p face.
//
// The face is cut out of the frame and scaled first, so that the
// embedding is computed from the very image that is stored with the
// enrolment: what the editor shows of a person is what was recognised.
//
// The landmarks that align the face are looked up in the cut-out itself
// and not taken from the frame. The frame is searched at a reduced size
// (--face-detect-width), so the landmarks of the frame are only precise
// to a fraction of a pixel of the frame, and the alignment they produce
// is off by a few pixels. SFace is very sensitive to that: two pixels of
// landmark error already move the vector by 0.03, and eight pixels by
// 0.2. The cut-out is the picture that is stored, and the face is bigger
// in it than it was in the frame, so the landmarks found in it are the
// accurate ones.
//
// The vector therefore depends on the stored picture alone and not on the
// distance at which the camera happened to show the face. That is what
// makes two sightings of the same person agree with each other -- and
// what makes the vector of a row reproducible from the picture of that
// row.
bool embedFace(const cv::Mat& source, const fd::FaceBox& face, int size,
               const fd::FaceDetector& detector,
               const fd::FaceRecognizer& recognizer,
               fd::Embedding& embedding) {
    FaceCrop crop;
    if (!cropFace(source, face, size, crop)) {
        return false;
    }

    const fd::Image image = toFaceImage(crop.image);

    fd::FaceBox found;
    if (faceInCutOut(image, crop.box, detector, found) &&
        recognizer.embed(image, found, embedding)) {
        return true;
    }

    // No face was found in the cut-out -- a cut-out that is too blurry,
    // perhaps. The landmarks of the frame are used then, which is what the
    // application did before.
    return recognizer.embed(image, crop.box, embedding);
}

// Scores a face against the *people* of the database.
//
// The score of a person is the mean distance to the few of its samples
// that are the closest to the face. That is steadier than the distance
// to the closest sample alone: the samples of one person disagree with
// each other -- they were taken in different sessions, with a different
// light and a different distance -- and the closest one of them is often
// an outlier.
//
// This is what tells a person who was enrolled twice apart from a person
// who is merely unknown, and it is the score that decides whether a new
// face belongs to somebody who is in the database already.
fd::FaceDatabase::PersonMatch matchPerson(const fd::FaceDatabase& database,
                                           const fd::Embedding& embedding,
                                           float maxDistance, int samples) {
    fd::PersonMatchOptions options;
    options.max_distance = maxDistance;
    options.samples = samples;
    return database.matchPerson(embedding, options);
}

// The groups of people that look like one person.
//
// Two people are put in the same group when they are close enough to
// each other. The groups are found with a union-find, so that people
// that meet through a third one end up in the same group as well: the
// people 1, 9 and 11 are one group even when 9 and 11 are further
// apart than 1 and 9.
//
// The groups are returned in the order of their first member, and the
// members of a group are in the order of the people.
std::vector<std::vector<int>> duplicateGroups(
    const fd::FaceDatabase& database,
    const std::vector<fd::FaceDatabase::PersonInfo>& people, float threshold,
    int samples) {
    std::vector<int> parent(people.size());
    for (std::size_t i = 0; i < parent.size(); ++i) {
        parent[i] = static_cast<int>(i);
    }

    // The root of a person is the person itself, or the root of the
    // group it was put in.
    const auto root = [&parent](int index) {
        int current = index;
        while (parent[static_cast<std::size_t>(current)] != current) {
            current = parent[static_cast<std::size_t>(current)];
        }
        // The path to the root is shortened, so that the search stays
        // short even for a long chain.
        int walk = index;
        while (parent[static_cast<std::size_t>(walk)] != current) {
            const int next = parent[static_cast<std::size_t>(walk)];
            parent[static_cast<std::size_t>(walk)] = current;
            walk = next;
        }
        return current;
    };

    for (std::size_t i = 0; i < people.size(); ++i) {
        for (std::size_t j = i + 1; j < people.size(); ++j) {
            const float distance =
                database.personDistance(people[i].person_id,
                                        people[j].person_id, samples);
            if (distance > threshold) {
                continue;
            }
            const int left = root(static_cast<int>(i));
            const int right = root(static_cast<int>(j));
            if (left != right) {
                parent[static_cast<std::size_t>(left)] = right;
            }
        }
    }

    // The groups, in the order of their first member.
    std::vector<int> order;
    std::vector<std::vector<int>> groups;
    for (std::size_t i = 0; i < people.size(); ++i) {
        const int leader = root(static_cast<int>(i));
        auto it = std::find(order.begin(), order.end(), leader);
        if (it == order.end()) {
            order.push_back(leader);
            groups.push_back({static_cast<int>(i)});
            continue;
        }
        groups[static_cast<std::size_t>(it - order.begin())].push_back(
            static_cast<int>(i));
    }

    // The groups of one person alone are not duplicates.
    std::vector<std::vector<int>> result;
    for (const std::vector<int>& group : groups) {
        if (group.size() > 1) {
            result.push_back(group);
        }
    }
    return result;
}

// The name of a person, looked up among all of its samples.
//
// The name belongs to the person and not to the sample that happened to
// be the closest one, so it is looked up the same way the stores look it
// up.
std::string nameOfPerson(const fd::FaceDatabase& database, int person_id) {
    std::string name;
    for (const fd::FaceEntry& entry : database.entries()) {
        if (entry.person_id != person_id || entry.info.user_name.empty()) {
            continue;
        }
        if (name.empty() || entry.info.user_name > name) {
            name = entry.info.user_name;
        }
    }
    return name;
}

// The metadata of a person, looked up among all of its samples.
//
// The metadata belongs to the person and not to the sample that happened
// to be the closest one, so it is looked up the same way the name is.
std::string metadataOfPerson(const fd::FaceDatabase& database, int person_id) {
    std::string metadata;
    for (const fd::FaceEntry& entry : database.entries()) {
        if (entry.person_id != person_id || entry.info.metadata.empty()) {
            continue;
        }
        // The first sample of the person decides, the same way the editor
        // shows it.
        metadata = entry.info.metadata;
        break;
    }
    return metadata;
}

// Recomputes the vector of every sample that has a picture.
//
// The vector of a sample is what the recogniser computed for the picture
// that is stored next to it. A sample that was enrolled with the
// landmarks of a detection in the frame holds a vector that does not
// describe the picture next to it, and it cannot be reproduced from it.
// That is what this repairs: the picture is decoded, the face is looked up
// in the picture itself, and the vector is computed again. The vector
// then depends on the picture alone, which is what makes two sightings of
// the same person agree with each other.
//
// The samples that have no picture, or whose picture holds no face, are
// left alone -- there is nothing to compute a vector from.
//
// Returns the number of samples whose vector was rewritten.
int reembedEntries(const std::vector<fd::FaceEntry>& entries,
                   std::vector<fd::FaceEntry>& out,
                   const fd::FaceDetector& detector,
                   const fd::FaceRecognizer& recognizer,
                   std::ostream& report) {
    out = entries;
    int rewritten = 0;
    for (fd::FaceEntry& entry : out) {
        if (entry.info.photo.empty()) {
            report << "  person " << entry.person_id
                   << ": no picture, the vector is left alone\n";
            continue;
        }

        const cv::Mat picture =
            cv::imdecode(entry.info.photo, cv::IMREAD_COLOR);
        if (picture.empty()) {
            report << "  person " << entry.person_id
                   << ": the picture could not be read, the vector is left "
                      "alone\n";
            continue;
        }

        fd::Image image;
        image.width = picture.cols;
        image.height = picture.rows;
        image.channels = 3;
        image.data.assign(picture.datastart, picture.dataend);

        // The picture is the cut-out of one face, so the face is the one
        // in the middle of it. A cut-out of a crowded scene can hold more
        // than one face, and the middle one is the one that was cut out.
        const std::vector<fd::FaceBox> faces = detector.detect(image);
        if (faces.empty()) {
            report << "  person " << entry.person_id
                   << ": no face in the picture, the vector is left alone\n";
            continue;
        }
        const int centreX = picture.cols / 2;
        const int centreY = picture.rows / 2;
        const fd::FaceBox* face = &faces.front();
        long bestDistance = 0;
        for (const fd::FaceBox& candidate : faces) {
            const long dx = static_cast<long>(candidate.x + candidate.width / 2) -
                            centreX;
            const long dy = static_cast<long>(candidate.y + candidate.height / 2) -
                            centreY;
            const long distance = dx * dx + dy * dy;
            if (&candidate == &faces.front() || distance < bestDistance) {
                face = &candidate;
                bestDistance = distance;
            }
        }

        fd::Embedding embedding;
        if (!recognizer.embed(image, *face, embedding)) {
            report << "  person " << entry.person_id
                   << ": the vector could not be computed, it is left alone\n";
            continue;
        }

        const float moved = entry.embedding.cosineDistance(embedding);
        entry.embedding = embedding;
        ++rewritten;
        report << "  person " << entry.person_id << ": the vector moved by "
               << formatDistance(moved) << '\n';
    }
    return rewritten;
}

// Draws a box and a caption for every detected face. A recognised person
// gets a green box, an unknown face an amber one and a face that is being
// watched by the tracker a blue one, so the overlay can be read at a
// glance.
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

        cv::Scalar color;
        switch (label.state) {
        case FaceLabel::State::Known:
            color = cv::Scalar(0, 220, 0);
            break;
        case FaceLabel::State::Tracking:
            color = cv::Scalar(255, 120, 0);
            break;
        default:
            color = cv::Scalar(0, 165, 255);
            break;
        }
        cv::rectangle(image,
                      cv::Rect(face.x, face.y, face.width, face.height),
                      color, thickness);

        if (text.empty()) {
            continue;
        }

        // The caption is drawn in a filled box of its own, above the face.
        // A face that is being watched has a second line: the number of
        // views that were collected for it so far.
        int baseline = 0;
        const cv::Size textSize =
            cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, fontScale,
                            thickness, &baseline);
        int detailWidth = 0;
        int detailHeight = 0;
        if (!label.detail.empty()) {
            int detailBaseline = 0;
            const cv::Size detailSize =
                cv::getTextSize(label.detail, cv::FONT_HERSHEY_SIMPLEX,
                                fontScale, thickness, &detailBaseline);
            detailWidth = detailSize.width;
            detailHeight = detailSize.height;
        }

        const int lineHeight = textSize.height + 6;
        const int captionHeight = lineHeight * (label.detail.empty() ? 1 : 2);
        const int captionWidth = std::max(textSize.width, detailWidth) + 8;
        const int textX = std::max(0, face.x);

        // Above the face, and below it when there is no room above.
        int top = face.y - captionHeight - 6;
        if (top < 0) {
            top = face.y + face.height + 6;
        }
        if (top + captionHeight > image.rows) {
            top = std::max(0, image.rows - captionHeight);
        }

        cv::rectangle(image,
                      cv::Rect(textX, top, captionWidth, captionHeight),
                      color, cv::FILLED);
        cv::putText(image, text,
                    cv::Point(textX + 4, top + textSize.height + 2),
                    cv::FONT_HERSHEY_SIMPLEX, fontScale, cv::Scalar(0, 0, 0),
                    thickness, cv::LINE_AA);
        if (!label.detail.empty()) {
            cv::putText(image, label.detail,
                        cv::Point(textX + 4,
                                 top + lineHeight + detailHeight + 2),
                        cv::FONT_HERSHEY_SIMPLEX, fontScale,
                        cv::Scalar(0, 0, 0), thickness, cv::LINE_AA);
        }
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
    const bool faceDbReembed = flag(argc, argv, "face-db-reembed");
    const bool faceDbDuplicates = flag(argc, argv, "face-db-duplicates");

    // The distance below which two people are reported as probably one
    // person.
    const double faceDbDuplicatesDistance =
        std::atof(arg(argc, argv, "face-db-duplicates-distance", "0.6")
                      .c_str());

    // The table of the PostgreSQL store. It is "face_enrollments" by
    // default; naming another one is what makes it possible to work on a
    // copy of the database without touching the real one.
    const std::string faceDbTable = arg(argc, argv, "face-db-table", "");

    // --face-db-merge=<from>:<to> moves the samples of one person to
    // another one. The "from" part may hold several ids, separated by
    // commas: --face-db-merge=9,10,13:1 moves three people to one.
    const std::string faceDbMergeStr = arg(argc, argv, "face-db-merge", "");
    std::vector<int> faceDbMergeFrom;
    int faceDbMergeTo = -1;
    if (!faceDbMergeStr.empty()) {
        const std::size_t colon = faceDbMergeStr.find(':');
        const std::string invalid =
            "Invalid --face-db-merge value '" + faceDbMergeStr +
            "' (expected <from>:<to>, e.g. 24:1 or 9,10,13:1).\n";
        if (colon == std::string::npos) {
            std::cerr << invalid;
            return 2;
        }
        try {
            faceDbMergeTo = std::stoi(faceDbMergeStr.substr(colon + 1));
        } catch (const std::exception&) {
            std::cerr << invalid;
            return 2;
        }
        if (faceDbMergeTo < 0) {
            std::cerr << "Invalid --face-db-merge value '" << faceDbMergeStr
                      << "' (the ids of the people are not negative).\n";
            return 2;
        }

        // The ids of the people whose samples are moved.
        const std::string from = faceDbMergeStr.substr(0, colon);
        std::size_t start = 0;
        while (start <= from.size()) {
            const std::size_t comma = from.find(',', start);
            const std::string part = from.substr(
                start, comma == std::string::npos ? std::string::npos
                                                  : comma - start);
            if (part.empty()) {
                std::cerr << invalid;
                return 2;
            }
            int id = -1;
            try {
                id = std::stoi(part);
            } catch (const std::exception&) {
                std::cerr << invalid;
                return 2;
            }
            if (id < 0) {
                std::cerr << "Invalid --face-db-merge value '"
                          << faceDbMergeStr
                          << "' (the ids of the people are not negative).\n";
                return 2;
            }
            if (id == faceDbMergeTo) {
                std::cerr << "Nothing to merge: --face-db-merge names the "
                             "same person twice.\n";
                return 2;
            }
            faceDbMergeFrom.push_back(id);
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }
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

    // How many samples of a person are averaged when a face is matched
    // against the people of the database. One is the distance to the
    // closest sample; more than one is steadier, and is what makes a
    // person who was enrolled several times recognisable.
    int faceMatchSamples = 3;
    {
        const std::string value = arg(argc, argv, "face-match-samples", "3");
        if (!parseInteger(value, faceMatchSamples) || faceMatchSamples < 1) {
            std::cerr << "Invalid --face-match-samples value '" << value
                      << "' (expected a number of samples, at least 1).\n";
            return 2;
        }
    }
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

    // The knobs of the deferred mode of --auto-enroll. A face that nobody
    // knows is watched for a while: the pictures and the embeddings of it
    // are collected, and it is written to the store when the application
    // is sure that all of them show the same person.
    const std::string autoEnrollMode =
        arg(argc, argv, "auto-enroll-mode", "deferred");
    if (autoEnrollMode != "deferred" && autoEnrollMode != "immediate") {
        std::cerr << "Invalid --auto-enroll-mode value '" << autoEnrollMode
                  << "' (expected 'deferred' or 'immediate').\n";
        return 2;
    }

    int autoEnrollSamples = 3;
    const std::string autoEnrollSamplesStr =
        arg(argc, argv, "auto-enroll-samples", "3");
    if (!parseInteger(autoEnrollSamplesStr, autoEnrollSamples) ||
        autoEnrollSamples < 1) {
        std::cerr << "Invalid --auto-enroll-samples value '"
                  << autoEnrollSamplesStr
                  << "' (expected a positive integer).\n";
        return 2;
    }

    double autoEnrollWindow = 20.0;
    const std::string autoEnrollWindowStr =
        arg(argc, argv, "auto-enroll-window", "20");
    if (!parseNumber(autoEnrollWindowStr, autoEnrollWindow) ||
        autoEnrollWindow < 0.0) {
        std::cerr << "Invalid --auto-enroll-window value '"
                  << autoEnrollWindowStr
                  << "' (expected a number of seconds).\n";
        return 2;
    }

    double autoEnrollTimeout = 10.0;
    const std::string autoEnrollTimeoutStr =
        arg(argc, argv, "auto-enroll-timeout", "10");
    if (!parseNumber(autoEnrollTimeoutStr, autoEnrollTimeout) ||
        autoEnrollTimeout < 0.0) {
        std::cerr << "Invalid --auto-enroll-timeout value '"
                  << autoEnrollTimeoutStr
                  << "' (expected a number of seconds).\n";
        return 2;
    }

    double autoEnrollConsistency = 0.35;
    const std::string autoEnrollConsistencyStr =
        arg(argc, argv, "auto-enroll-consistency", "0.35");
    if (!parseNumber(autoEnrollConsistencyStr, autoEnrollConsistency) ||
        autoEnrollConsistency < 0.0) {
        std::cerr << "Invalid --auto-enroll-consistency value '"
                  << autoEnrollConsistencyStr
                  << "' (expected a distance).\n";
        return 2;
    }

    double autoEnrollDiversity = 0.08;
    const std::string autoEnrollDiversityStr =
        arg(argc, argv, "auto-enroll-diversity", "0.08");
    if (!parseNumber(autoEnrollDiversityStr, autoEnrollDiversity) ||
        autoEnrollDiversity < 0.0) {
        std::cerr << "Invalid --auto-enroll-diversity value '"
                  << autoEnrollDiversityStr
                  << "' (expected a distance).\n";
        return 2;
    }

    int autoEnrollMinSize = 64;
    const std::string autoEnrollMinSizeStr =
        arg(argc, argv, "auto-enroll-min-size", "64");
    if (!parseInteger(autoEnrollMinSizeStr, autoEnrollMinSize) ||
        autoEnrollMinSize < 0) {
        std::cerr << "Invalid --auto-enroll-min-size value '"
                  << autoEnrollMinSizeStr
                  << "' (expected a number of pixels).\n";
        return 2;
    }

    // The two gates of the tracker pull in opposite directions: the views
    // of a candidate have to be the same person (within the consistency)
    // but not the same picture (further apart than the diversity). When the
    // diversity is not smaller than the consistency, no view could ever be
    // added to a candidate, so the diversity is lowered.
    if (autoEnrollDiversity >= autoEnrollConsistency) {
        std::cerr << "Ignoring --auto-enroll-diversity="
                  << autoEnrollDiversity
                  << ": it has to be smaller than --auto-enroll-consistency ("
                  << autoEnrollConsistency << ").\n";
        autoEnrollDiversity = std::max(0.0, autoEnrollConsistency - 0.01);
    }

    const bool autoEnrollStrict = flag(argc, argv, "auto-enroll-strict");
    const bool autoEnrollAttach = flag(argc, argv, "auto-enroll-attach");

    // How many samples a person may hold before the faces that look like
    // it are no longer added to it. A person that is described by a
    // handful of good pictures does not need every sighting of it, and a
    // database that grows without a limit is a database nobody can look
    // at. Zero means no limit.
    int autoEnrollAttachMax = 20;
    {
        const std::string value =
            arg(argc, argv, "auto-enroll-attach-max", "20");
        if (!parseInteger(value, autoEnrollAttachMax) ||
            autoEnrollAttachMax < 0) {
            std::cerr << "Invalid --auto-enroll-attach-max value '" << value
                      << "' (expected a number of samples, 0 for no limit).\n";
            return 2;
        }
    }
    const bool autoEnrollVerbose = flag(argc, argv, "auto-enroll-verbose");

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
        faceDbList || faceDbInit || faceDbRemoveId >= 0 || faceDbReembed ||
        faceDbDuplicates || !faceDbMergeFrom.empty();
    if (dbAdmin) {
#ifdef OC_HAVE_FACE
        if (faceDbTarget.empty()) {
            std::cerr << "--face-db-list, --face-db-init, --face-db-remove, "
                         "--face-db-reembed, --face-db-duplicates and "
                         "--face-db-merge need --face-db=<target>.\n";
            return 2;
        }

        std::string error;
        auto store = openFaceStore(faceDbTarget, faceDbTable, error);
        if (!store) {
            std::cerr << (error.empty()
                              ? std::string("Could not open the face "
                                            "database '") + faceDbTarget + "'."
                              : error)
                      << '\n';
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

        // --face-db-merge=<from>:<to>: the samples of one or more people
        // are moved to another one. The rows keep their identity and the
        // time the face was enrolled; only the person they belong to
        // changes.
        if (!faceDbMergeFrom.empty()) {
            std::vector<fd::FaceEntry> entries;
            if (!store->load(entries, error)) {
                std::cerr << "Could not read the face database: " << error
                          << '\n';
                return 1;
            }

            // The name of the person: the one of the target, or the one of
            // a source when the target does not have a name.
            std::string name;
            for (const fd::FaceEntry& entry : entries) {
                if (entry.person_id == faceDbMergeTo &&
                    !entry.info.user_name.empty()) {
                    name = entry.info.user_name;
                    break;
                }
            }
            if (name.empty()) {
                for (const int from : faceDbMergeFrom) {
                    for (const fd::FaceEntry& entry : entries) {
                        if (entry.person_id == from &&
                            !entry.info.user_name.empty()) {
                            name = entry.info.user_name;
                            break;
                        }
                    }
                    if (!name.empty()) {
                        break;
                    }
                }
            }

            // The people whose samples are moved.
            const auto isSource = [&faceDbMergeFrom](int person_id) {
                return std::find(faceDbMergeFrom.begin(), faceDbMergeFrom.end(),
                                 person_id) != faceDbMergeFrom.end();
            };

            int moved = 0;
            int kept = 0;
            std::vector<fd::FaceEntry> changed;
            for (fd::FaceEntry& entry : entries) {
                if (entry.person_id != faceDbMergeTo && !isSource(entry.person_id)) {
                    continue;
                }
                if (isSource(entry.person_id)) {
                    // The metadata of a person says where it came from,
                    // so that the origin of the samples is not lost. A
                    // sample that was moved before keeps the id it came
                    // from: that is the more useful of the two.
                    if (!hasJsonField(entry.info.metadata,
                                      "merged_from_person_id")) {
                        entry.info.metadata = addJsonField(
                            entry.info.metadata, "merged_from_person_id",
                            std::to_string(entry.person_id));
                    }
                    entry.person_id = faceDbMergeTo;
                    ++moved;
                } else {
                    ++kept;
                }
                // The name and the metadata are properties of the person,
                // so every sample of the person carries them.
                entry.info.user_name = name;
                changed.push_back(entry);
            }

            if (moved == 0) {
                std::cerr << "None of the people of --face-db-merge is in "
                          << faceDbTarget << ".\n";
                return 1;
            }

            if (!store->update(changed, error)) {
                std::cerr << "Could not write the face database: " << error
                          << '\n';
                return 1;
            }
            std::cout << "Moved " << moved << " sample(s) to person "
                      << faceDbMergeTo << " (" << kept
                      << " sample(s) were there already).\n";
            return 0;
        }

        // --face-db-duplicates: the people that look like one person.
        //
        // A face that was enrolled twice -- in two sessions, with another
        // light and another distance -- ends up as two people in the
        // database. The report lists the pairs of people that are close
        // enough to be the same person, so that they can be merged with
        // --face-db-merge.
        if (faceDbDuplicates) {
            // A database that holds nobody has no duplicates. The list
            // is asked first, because it answers for a database that does
            // not exist yet, while the load does not.
            std::vector<fd::PersonSummary> summaries;
            if (!store->list(summaries, error)) {
                std::cerr << "Could not read the face database: " << error
                          << '\n';
                return 1;
            }
            if (summaries.empty()) {
                std::cout << "No two people of " << faceDbTarget
                          << " look like one person (distance below "
                          << faceDbDuplicatesDistance << ").\n";
                return 0;
            }

            std::vector<fd::FaceEntry> entries;
            if (!store->load(entries, error)) {
                std::cerr << "Could not read the face database: " << error
                          << '\n';
                return 1;
            }

            fd::FaceDatabase database;
            for (const fd::FaceEntry& entry : entries) {
                database.add(entry);
            }

            const auto describe = [](const fd::FaceDatabase::PersonInfo& info) {
                std::string text = "person " + std::to_string(info.person_id) +
                                   " (" + std::to_string(info.samples) +
                                   " sample(s)";
                if (!info.user_name.empty()) {
                    text += ", " + info.user_name;
                }
                return text;
            };

            const std::vector<fd::FaceDatabase::PersonInfo> people =
                database.people();
            const std::vector<std::vector<int>> groups = duplicateGroups(
                database, people, static_cast<float>(faceDbDuplicatesDistance),
                faceMatchSamples);

            for (const std::vector<int>& group : groups) {
                std::cout << "  ";
                for (std::size_t k = 0; k < group.size(); ++k) {
                    if (k > 0) {
                        std::cout << ", ";
                    }
                    std::cout << describe(people[static_cast<std::size_t>(
                        group[k])]);
                }
                std::cout << "\n";

                // The distance of every member of the group to the first
                // one, so that the operator can see how sure the group is.
                for (std::size_t k = 1; k < group.size(); ++k) {
                    const float distance = database.personDistance(
                        people[static_cast<std::size_t>(group[0])].person_id,
                        people[static_cast<std::size_t>(group[k])].person_id,
                        faceMatchSamples);
                    std::cout << "      person "
                              << people[static_cast<std::size_t>(group[k])]
                                     .person_id
                              << " is at distance " << formatDistance(distance)
                              << " from person "
                              << people[static_cast<std::size_t>(group[0])]
                                     .person_id
                              << "\n";
                }
            }

            if (groups.empty()) {
                std::cout << "No two people of " << faceDbTarget
                          << " look like one person (distance below "
                          << faceDbDuplicatesDistance << ").\n";
            } else {
                std::cout << groups.size() << " group(s) of " << faceDbTarget
                          << " look like one person (distance below "
                          << faceDbDuplicatesDistance
                          << "). Merge them with --face-db-merge.\n";
            }
            return 0;
        }

        // --face-db-reembed: the vectors are computed again from the
        // pictures that are stored with them.
        if (faceDbReembed) {
#ifdef OC_HAVE_OPENCV
            fd::FaceDetector detector;
            fd::FaceDetector::Options detectorOptions;
            detectorOptions.model_path = faceModel;
            detectorOptions.score_threshold = static_cast<float>(faceScore);
            detectorOptions.detection_width = faceDetectWidth;
            if (!detector.open(detectorOptions)) {
                std::cerr << "Could not load the face-detection model '"
                          << faceModel << "'.\n";
                return 1;
            }

            fd::FaceRecognizer recognizer;
            fd::FaceRecognizer::Options recognizerOptions;
            recognizerOptions.model_path = faceRecModel;
            if (!recognizer.open(recognizerOptions)) {
                std::cerr << "Could not load the face-recognition model '"
                          << faceRecModel << "'.\n";
                return 1;
            }

            std::vector<fd::FaceEntry> entries;
            if (!store->load(entries, error)) {
                std::cerr << "Could not read the face database: " << error
                          << '\n';
                return 1;
            }
            if (entries.empty()) {
                std::cout << "There is nothing to repair in " << faceDbTarget
                          << ".\n";
                return 0;
            }

            std::vector<fd::FaceEntry> repaired;
            const int rewritten =
                reembedEntries(entries, repaired, detector, recognizer,
                               std::cout);
            if (rewritten > 0) {
                // The rows are changed in place, so that they keep their
                // identity and the time the face was enrolled.
                if (!store->update(repaired, error)) {
                    std::cerr << "Could not write the face database: " << error
                              << '\n';
                    return 1;
                }
            }
            std::cout << "Rewrote " << rewritten << " vector(s) out of "
                      << entries.size() << " sample(s) in " << faceDbTarget
                      << ".\n";
            return 0;
#else
            std::cerr << "--face-db-reembed needs a build with OpenCV.\n";
            return 1;
#endif
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
        std::cerr << "--face-db-list, --face-db-init, --face-db-remove and "
                     "--face-db-reembed require a build with "
                     "OBJECTIVE_CONTROL_ENABLE_FACE=ON.\n";
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
            faceStore = openFaceStore(faceDbTarget, faceDbTable, error);
            if (!faceStore) {
                std::cerr << (error.empty()
                                  ? std::string("Could not open the face "
                                                "database '") + faceDbTarget +
                                        "'."
                                  : error)
                          << '\n';
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

    // How many samples of every person the in-memory database holds. It is
    // what tells the enrichment whether a person is already collected
    // enough.
    std::map<int, int> sampleCounts;
    for (const fd::FaceEntry& entry : faceDatabase.entries()) {
        ++sampleCounts[entry.person_id];
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

    // ---------------- The tracker of the unknown faces ----------------
    //
    // The deferred mode of --auto-enroll watches the faces that are not in
    // the database yet: the pictures and the embeddings of a face are
    // collected in memory, and the person is written to the store when the
    // application is sure that all of the views show the same person.
    //
    // The immediate mode is the behaviour of the earlier versions: a face
    // that is not in the database is enrolled as soon as it is seen.
    const bool deferredEnroll = autoEnroll && autoEnrollMode == "deferred";

    oc::AutoEnroller::Options trackerOptions;
    trackerOptions.target_samples = autoEnrollSamples;
    trackerOptions.window_us =
        static_cast<std::uint64_t>(autoEnrollWindow * 1000000.0);
    trackerOptions.timeout_us =
        static_cast<std::uint64_t>(autoEnrollTimeout * 1000000.0);
    trackerOptions.consistency = static_cast<float>(autoEnrollConsistency);
    trackerOptions.diversity = static_cast<float>(autoEnrollDiversity);
    trackerOptions.min_size = autoEnrollMinSize;
    // The detector computes a score of its own, and the gate of the
    // tracker is the knob the application already has.
    trackerOptions.min_score = static_cast<float>(faceScore);
    trackerOptions.strict = autoEnrollStrict;
    oc::AutoEnroller tracker(trackerOptions);

    // What the deferred mode did.
    int trackedFaces = 0;   // the faces that were watched
    int storedFaces = 0;    // the faces that were written to the store
    int discardedFaces = 0; // the faces that were thrown away
    int enrolledPeople = 0; // the people that were written to the store
    int enrolledViews = 0;  // the views that were written to the store
    int attachedViews = 0;  // the views that were added to a known person

    // The person id of a candidate. It is remembered, so that a store that
    // could not be written is tried again with the same id instead of
    // burning a new one on every frame.
    std::map<int, int> candidateIds;

    // The person a candidate belongs to when its views are added to a
    // person that is enrolled already (--auto-enroll-attach).
    std::map<int, int> attachTargets;

    // The candidates whose store could not be written. The error is
    // printed once, and not once per frame.
    std::set<int> writeFailures;

    // What was found out about one face of a frame.
    struct FaceResult {
        fd::FaceBox box;

        /// True when the face is in the database.
        bool known = false;

        /// True when the face is not in the database, but is close enough
        /// to one of its samples to be very likely the same person.
        bool probable = false;

        /// True when the face is being watched by the tracker.
        bool tracking = false;

        /// The candidate that watches the face.
        int candidate = -1;

        /// The number of views the candidate holds.
        int views = 0;

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

    // Adds a sample to the in-memory database and keeps the count of the
    // samples of that person up to date.
    auto addToDatabase = [&](const fd::FaceEntry& entry) {
        if (faceDatabase.add(entry)) {
            ++sampleCounts[entry.person_id];
        }
    };

    // Writes the views of a candidate to the store.
    //
    // The person id is allocated here and not when the candidate was
    // started, so that a face that is never stored does not burn an id.
    auto commitCandidate = [&](const oc::FaceCandidate& candidate) {
        if (faceStore == nullptr) {
            // Without a store there is nowhere to put the views.
            return;
        }

        const int candidateId = candidate.id;
        const int viewCount = static_cast<int>(candidate.views.size());
        const float confidence = candidate.confidence();

        // Somebody else may have enrolled the same person in the
        // meantime: the views are matched against the database one last
        // time, so that a person is not stored twice.
        //
        // Two questions are asked. The first one -- the closest sample -- is
        // strict: the face *is* the person of that sample. The second one --
        // the person-level score -- is forgiving: the face looks like that
        // person, even though no single sample of that person matches it as
        // well as the threshold demands. That is the case when the person
        // was enrolled in another session, with another light and another
        // distance, and it is exactly the case that used to enrol somebody
        // who was in the database already a second time.
        int existing = -1;
        int probable = -1;
        float probableDistance = 1.0f;
        for (const oc::FaceView& view : candidate.views) {
            const fd::FaceDatabase::Match match = faceDatabase.match(
                view.embedding, static_cast<float>(maxDistance));
            if (match.matched) {
                existing = match.person_id;
                break;
            }
            const fd::FaceDatabase::PersonMatch person = matchPerson(
                faceDatabase, view.embedding, static_cast<float>(sameDistance),
                faceMatchSamples);
            if (person.matched && person.distance < probableDistance) {
                probable = person.person_id;
                probableDistance = person.distance;
            }
        }

        const auto target = attachTargets.find(candidateId);
        bool attaches = target != attachTargets.end();

        if (existing >= 0 && !attaches) {
            // The person is in the database already, so the candidate is
            // thrown away and nothing is written.
            tracker.remove(candidateId);
            candidateIds.erase(candidateId);
            ++discardedFaces;
            std::cout << "  Candidate " << candidateId
                      << " discarded: person " << existing
                      << " is in the database already.\n";
            return;
        }

        // The person the views belong to.
        int personId = 0;
        if (attaches) {
            personId = target->second;
        } else if (existing >= 0) {
            personId = existing;
        } else if (probable >= 0) {
            // The views look like somebody who is in the database already.
            // They are added to that person instead of enrolling a second
            // person: the person gets more material to be recognised with,
            // and the database does not fill up with copies of the same
            // people.
            personId = probable;
            attaches = true;
        } else {
            const auto known = candidateIds.find(candidateId);
            if (known != candidateIds.end()) {
                // The id of an earlier attempt: the same id is used again,
                // so that a store that could not be written does not burn
                // an id per frame.
                personId = known->second;
            } else {
                personId = allocatePersonId();
                candidateIds[candidateId] = personId;
            }
        }

        // The samples of the person: one row per view, with the metadata
        // of the person on every one of them.
        std::vector<fd::FaceEntry> entries =
            oc::makeEnrolmentEntries(candidate, personId);

        // The name of the person belongs to the person, so every sample
        // of the person carries it -- the samples that are added to a
        // person that is in the database already included.
        const std::string name = nameOfPerson(faceDatabase, personId);
        if (!name.empty()) {
            for (fd::FaceEntry& entry : entries) {
                entry.info.user_name = name;
            }
        }

        // The same goes for the metadata: it is a property of the
        // person, so the samples that are added to a person that is in
        // the database already carry the metadata of that person and
        // not the metadata of the candidate.
        if (attaches) {
            const std::string metadata =
                metadataOfPerson(faceDatabase, personId);
            if (!metadata.empty()) {
                for (fd::FaceEntry& entry : entries) {
                    entry.info.metadata = metadata;
                }
            }
        }

        // The views are written in one go: the store writes the samples of
        // one person in one transaction (PostgreSQL) or in one rewrite
        // (the file), so the person is either stored with all of its views
        // or not at all.
        std::string error;
        if (!faceStore->addAll(entries, error)) {
            if (writeFailures.insert(candidateId).second) {
                std::cerr << "  Could not store person " << personId << " in '"
                          << faceDbTarget << "': " << error << '\n';
            }
            if (!attaches) {
                // The person is new: whatever reached the store is taken
                // back, so that a person is either stored with all of its
                // views or not at all. The person that was already in the
                // database is left alone -- its own samples are not ours
                // to remove.
                std::string ignored;
                faceStore->remove(personId, ignored);
            }
            // The candidate stays in the tracker: the store is tried again
            // with the next frame.
            return;
        }
        writeFailures.erase(candidateId);

        // The samples are added to the in-memory database, so that the
        // person is recognised from the next frame on and the matcher uses
        // all of the views from now on.
        for (const fd::FaceEntry& entry : entries) {
            addToDatabase(entry);
        }

        tracker.remove(candidateId);
        candidateIds.erase(candidateId);
        attachTargets.erase(candidateId);

        ++storedFaces;
        if (attaches) {
            attachedViews += viewCount;
            std::cout << "  Added " << viewCount << " view(s) to person "
                      << personId << " (confidence "
                      << formatDistance(confidence) << ").\n";
        } else {
            ++enrolledPeople;
            enrolledViews += viewCount;
            std::cout << "  Enrolled person " << personId << " with "
                      << viewCount << " view(s) (confidence "
                      << formatDistance(confidence) << ").\n";
        }
    };

    // Reports the candidates that ran out of time and stores the ones that
    // are ready.
    auto updateCandidates = [&](std::uint64_t nowUs) {
        // The candidates that were not seen for too long, and the ones
        // whose collection window is over.
        for (const oc::AutoEnroller::Decision& decision :
             tracker.expire(nowUs)) {
            if (decision.kind !=
                oc::AutoEnroller::Decision::Kind::CandidateDiscarded) {
                continue;
            }
            ++discardedFaces;
            // The tracker dropped the candidate, so nothing of it is
            // left to remember.
            candidateIds.erase(decision.candidate);
            attachTargets.erase(decision.candidate);
            std::cout << "  Candidate " << decision.candidate << " discarded: "
                      << decision.views << " view(s), "
                      << trackerReason(decision.reason) << ".\n";
        }

        // The candidates that are ready to be stored. Their ids are taken
        // first: storing a candidate takes it out of the tracker, and the
        // pointers would not survive that.
        std::vector<int> ready;
        for (const oc::FaceCandidate* candidate : tracker.ready()) {
            ready.push_back(candidate->id);
        }
        for (const int id : ready) {
            const oc::FaceCandidate* candidate = tracker.candidate(id);
            if (candidate != nullptr) {
                commitCandidate(*candidate);
            }
        }
    };

    // Offers a face to the tracker and records what happened.
    //
    // @p owner is the candidate the face belongs to -- nullptr when it
    // belongs to none -- and @p distance is how far the face is from it.
    auto observeFace =
        [&](const fd::FaceBox& face, const fd::Embedding& embedding,
            const oc::FaceCandidate* owner, float distance,
            const std::function<std::vector<std::uint8_t>(const fd::FaceBox&)>&
                pictureOf,
            std::uint64_t nowUs, FaceResult& result,
            std::vector<std::string>& notes) {
        oc::FaceView view;
        view.embedding = embedding;
        view.width = cropWidth(face);
        view.height = cropHeight(face);
        view.detection_score = face.score;
        view.timestamp_us = nowUs;
        if (pictureOf) {
            view.photo = pictureOf(face);
        }

        const oc::AutoEnroller::Decision decision =
            tracker.observe(view, nowUs);

        // The face belongs to a candidate when the tracker says so, even
        // when the view was not kept: the face is being watched all the
        // same.
        if (decision.candidate >= 0) {
            result.tracking = true;
            result.candidate = decision.candidate;
            result.views = decision.views;
        }

        switch (decision.kind) {
        case oc::AutoEnroller::Decision::Kind::CandidateStarted:
            ++trackedFaces;
            notes.push_back("  Tracking a new face (candidate " +
                            std::to_string(decision.candidate) + ").");
            break;
        case oc::AutoEnroller::Decision::Kind::ViewAccepted:
            if (autoEnrollVerbose) {
                std::string note = "  View accepted: candidate " +
                                   std::to_string(decision.candidate) + ", " +
                                   std::to_string(decision.views) +
                                   " view(s)";
                if (owner != nullptr) {
                    note += ", distance " + formatDistance(distance);
                }
                notes.push_back(note + ".");
            }
            break;
        case oc::AutoEnroller::Decision::Kind::ViewRejected:
            if (autoEnrollVerbose) {
                notes.push_back("  View rejected: " +
                                trackerReason(decision.reason) + ".");
            }
            break;
        default:
            break;
        }
        return decision;
    };

    // Detects the faces in a frame and works out who each of them is.
    //
    // A face that is in the database already is left alone. A face that is
    // not in the database is offered to the tracker, which watches it until
    // it is recognised with high confidence: only then is the person
    // written to the store.
    //
    // @p pictureOf makes the picture of a face. It is empty when the build
    // cannot make one.
    //
    // @p embedOf computes the embedding of a face. It is empty when the
    // build cannot make one, and the embedding is then computed from the
    // whole frame instead.
    auto analyseFaces =
        [&](const fd::Image& image, std::uint64_t nowUs,
            const std::function<std::vector<std::uint8_t>(const fd::FaceBox&)>&
                pictureOf,
            const std::function<bool(const fd::FaceBox&, fd::Embedding&)>&
                embedOf,
            std::vector<std::string>& notes) -> std::vector<FaceResult> {
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

            // The same face, scored against the *people* of the database
            // rather than against the single samples. It is the score
            // that recognises a person who was enrolled in another
            // session, where no single sample is close enough.
            const fd::FaceDatabase::PersonMatch person = matchPerson(
                faceDatabase, embedding, static_cast<float>(sameDistance),
                faceMatchSamples);

            // ---------------- Rule 1: the face is in the database ----------------
            if (match.matched) {
                // The face is in the database already. Nothing is stored
                // for it, whatever its person is called.
                result.known = true;
                result.person_id = match.person_id;
                result.user_name = match.user_name;

                // The enrichment of the proposal: the views of a person
                // that is enrolled already are collected as well, so that
                // the person gets more samples. It stops when the person
                // holds as many samples as the tracker collects views.
                if (deferredEnroll && autoEnrollAttach &&
                    sampleCounts[match.person_id] < autoEnrollSamples) {
                    const oc::AutoEnroller::Decision decision =
                        observeFace(face, embedding, nullptr, 0.0f, pictureOf,
                                    nowUs, result, notes);
                    if (decision.kind ==
                        oc::AutoEnroller::Decision::Kind::CandidateStarted) {
                        attachTargets[decision.candidate] = match.person_id;
                    }
                }
                results.push_back(result);
                continue;
            }

            if (!deferredEnroll) {
                // ---------------- The immediate mode ----------------
                // The face is not in the database -- but it is close enough
                // to one of its samples to be very likely the same person.
                // A second enrolment of somebody who is enrolled already
                // would fill the database with copies of them, so such a
                // face is reported and left alone.
                if (autoEnroll && faceStore && person.matched) {
                    result.probable = true;
                    result.person_id = person.person_id;
                    result.user_name = person.user_name;
                    results.push_back(result);
                    continue;
                }

                // The face is not in the database.
                if (autoEnroll && faceStore) {
                    // The properties of the new person: the name is left
                    // empty on purpose, so that it can be filled in later
                    // with the face database editor. The metadata says
                    // where the person came from.
                    fd::EnrolmentInfo info;
                    info.metadata = "{\"auto_enrolled\": true}";
                    if (pictureOf) {
                        info.photo = pictureOf(face);
                    }

                    const int personId = allocatePersonId();
                    fd::FaceEntry entry;
                    if (faceDatabase.enroll(personId, embedding, info,
                                            &entry)) {
                        std::string error;
                        if (faceStore->add(entry, error)) {
                            result.known = true;
                            result.person_id = personId;
                            result.enrolled = true;
                            result.distance = 0.0f;
                            ++sampleCounts[personId];
                            std::cout << "  Enrolled a new face as person "
                                      << personId << ".\n";
                        } else {
                            // The face is not in the database after all, so
                            // it is taken out of it again: it is enrolled by
                            // the next frame that shows it.
                            faceDatabase.remove(personId);
                            std::cerr << "  Could not store the new person "
                                      << personId << " in '" << faceDbTarget
                                      << "': " << error << '\n';
                        }
                    }
                }

                results.push_back(result);
                continue;
            }

            // ---------------- The deferred mode ----------------

            // Rule 2: does the face belong to a candidate that is already
            // being watched?
            const oc::FaceCandidate* owner = tracker.closest(
                embedding, static_cast<float>(autoEnrollConsistency));
            const float ownerDistance =
                owner != nullptr ? owner->closestDistance(embedding) : 0.0f;

            if (owner == nullptr) {
                // Rule 3: the face is close to somebody who is enrolled
                // already, but it is not that person.
                //
                // The face is watched all the same, and its views are
                // added to that person. A face that is probably somebody
                // who is enrolled already is exactly the material that
                // person needs: the camera sees a face differently every
                // time, and a person is only recognised from the
                // pictures that are in the database. Dropping those faces
                // is what left the database with a handful of samples per
                // person -- and what filled it with copies of the same
                // people.
                if (faceStore && person.matched) {
                    result.probable = true;
                    result.person_id = person.person_id;
                    result.user_name = person.user_name;

                    // A person that is described by enough pictures does
                    // not need every sighting of it.
                    if (autoEnrollAttachMax == 0 ||
                        sampleCounts[person.person_id] < autoEnrollAttachMax) {
                        const oc::AutoEnroller::Decision decision =
                            observeFace(face, embedding, nullptr, 0.0f,
                                        pictureOf, nowUs, result, notes);
                        if (decision.kind ==
                            oc::AutoEnroller::Decision::Kind::
                                CandidateStarted) {
                            attachTargets[decision.candidate] =
                                person.person_id;
                        }
                    }
                    results.push_back(result);
                    continue;
                }
            }

            // Rule 2 or rule 4: the face is offered to the tracker. It
            // either joins a candidate or starts one of its own.
            observeFace(face, embedding, owner, ownerDistance, pictureOf,
                        nowUs, result, notes);
            results.push_back(result);
            continue;
        }
        return results;
    };
#endif

#if defined(OC_HAVE_FACE) && defined(OC_HAVE_OPENCV)
    // Detects the faces in a frame and draws them, labelling each one with
    // the person id when the face is known. Returns the number of faces.
    auto annotateFrame = [&](cv::Mat& image, std::uint64_t nowUs) -> int {
        const fd::Image faceImage = toFaceImage(image);
        std::vector<std::string> notes;
        const std::vector<FaceResult> results = analyseFaces(
            faceImage, nowUs,
            [&](const fd::FaceBox& box) {
                return encodeFace(image, box, faceImageSize);
            },
            [&](const fd::FaceBox& box, fd::Embedding& embedding) {
                if (!faceRecognizer) {
                    return false;
                }
                return embedFace(image, box, faceImageSize, *faceDetector,
                                 *faceRecognizer, embedding);
            },
            notes);

        for (const std::string& note : notes) {
            std::cout << note << '\n';
        }

        // The faces that ran out of time are reported and the ones that
        // are ready are written to the store.
        updateCandidates(nowUs);

        std::vector<fd::FaceBox> boxes;
        std::vector<FaceLabel> labels;
        boxes.reserve(results.size());
        labels.reserve(results.size());
        for (const FaceResult& result : results) {
            boxes.push_back(result.box);

            FaceLabel label;
            if (result.known) {
                // The name of the person is shown when there is one; the
                // id is the fallback.
                label.state = FaceLabel::State::Known;
                label.text = result.user_name.empty()
                                 ? "person " + std::to_string(result.person_id)
                                 : result.user_name;
            } else if (result.probable) {
                // Somebody who is enrolled already, most likely. The name
                // carries a question mark and the box stays amber, because
                // the match is not certain.
                label.state = FaceLabel::State::Probable;
                const std::string who =
                    result.user_name.empty()
                        ? "person " + std::to_string(result.person_id)
                        : result.user_name;
                label.text = who + "?";
            } else if (result.tracking) {
                // The face is being watched: the box is blue and the
                // caption says how many views were collected so far, so
                // that the operator can see that the application is still
                // deciding.
                label.state = FaceLabel::State::Tracking;
                label.text = "tracking";
                label.detail = std::to_string(result.views) +
                               (result.views == 1 ? " view" : " views");
            } else {
                label.state = FaceLabel::State::Unknown;
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
                            annotateFrame(out, nowMicroseconds());
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
                return embedFace(view, box, faceImageSize, *faceDetector,
                                 *faceRecognizer, embedding);
            };
#endif

            const std::uint64_t nowUs = nowMicroseconds();

            std::vector<std::string> notes;
            const std::vector<FaceResult> results =
                analyseFaces(image, nowUs, pictureOf, embedOf, notes);

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
                } else if (result.tracking) {
                    // The face is not in the database yet: the application
                    // is watching it and collects the views of it, so it
                    // is not written down before it is sure.
                    std::cout << " -> tracking a new face (" << result.views
                              << (result.views == 1 ? " view" : " views")
                              << ")";
                } else if (faceDatabase.empty()) {
                    std::cout << " -> unknown (database is empty)";
                } else {
                    std::cout << " -> unknown (distance " << result.distance
                              << ")";
                }
            }
            std::cout << '\n';

            // What the tracker has to say about the faces of this frame.
            for (const std::string& note : notes) {
                std::cout << note << '\n';
            }

            // The faces that ran out of time are reported and the ones
            // that are ready are written to the store.
            updateCandidates(nowUs);

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

                    // The embedding is computed from the cut-out, the way
                    // the automatic enrolment computes it, so that a face
                    // that is enrolled by hand and a face that is enrolled
                    // automatically are recognised the same way.
                    fd::Embedding embedding;
                    bool embedded = false;
#ifdef OC_HAVE_OPENCV
                    {
                        cv::Mat view;
                        if (toMat(*frame, view) && faceDetector) {
                            embedded = embedFace(view, box, faceImageSize,
                                                 *faceDetector, *faceRecognizer,
                                                 embedding);
                        }
                    }
#endif
                    if (!embedded) {
                        embedded = faceRecognizer->embed(image, box, embedding);
                    }

                    fd::FaceEntry entry;
                    if (embedded &&
                        faceDatabase.enroll(enrollId, embedding, info, &entry)) {
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
        if (deferredEnroll) {
            // The deferred mode watches the faces first and writes them
            // down when it is sure: the report says how many of them
            // were stored and how many were thrown away.
            std::cout << "Tracked " << trackedFaces << " face(s); "
                      << storedFaces << " were stored, " << discardedFaces
                      << " were discarded.\n";
            std::cout << "Enrolled " << enrolledPeople
                      << " new person(s) with " << enrolledViews
                      << " view(s) in " << faceDbTarget << ".\n";
            if (attachedViews > 0) {
                std::cout << "Added " << attachedViews
                          << " view(s) to the people that were enrolled "
                             "already.\n";
            }
        } else {
            std::cout << "Enrolled " << autoEnrolledSeen << " new face(s) in "
                      << faceDbTarget << ".\n";
        }
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
