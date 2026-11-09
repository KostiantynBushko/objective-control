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

    // ---------------- Single/limited frame-capture path ----------------
    int received = 0;
    for (int attempts = 0; attempts < frameCount * 40; ++attempts) {
        auto frame = camera->grab();
        if (!frame) {
            continue;
        }

        std::cout << "Frame " << received << ": " << frame->width << 'x'
                  << frame->height << " (" << frame->channels
                  << " ch), " << frame->data.size() << " bytes, t="
                  << frame->timestamp_us << "us.\n";

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
    return 0;
}
