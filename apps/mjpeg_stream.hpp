#pragma once

// Minimal MJPEG-over-HTTP streaming server (no external dependencies beyond
// OpenCV's JPEG encoder). Used by the demo app to stream live camera frames to
// a web browser at http://<host>:<port>/ (or /stream for the raw image stream).
//
// Any number of clients may watch at once. Frames are produced by a single
// thread and fanned out to every connected client; a client that falls behind
// receives the newest frame instead of a growing backlog, so slow viewers
// never stall the stream. Because only one thread ever touches the camera,
// the frame callback does not have to be reentrant.
//
// Intended only for local/trusted networks — there is no authentication, TLS,
// or security hardening.

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace apps {

// Holds the most recently encoded frame and hands it to every waiting client.
class FrameBroadcaster {
public:
    using FramePtr = std::shared_ptr<const std::vector<unsigned char>>;

    // Producer side: store a new frame and wake every waiting client.
    void publish(FramePtr frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_ = std::move(frame);
            ++generation_;
        }
        cv_.notify_all();
    }

    // Consumer side: block until a frame newer than @p lastSeen exists.
    // Returns false once the server is shutting down.
    bool await(std::uint64_t lastSeen, FramePtr& frame,
               std::uint64_t& generation) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, lastSeen] {
            return generation_ != lastSeen || done_;
        });
        if (done_) {
            return false;
        }
        frame = frame_;
        generation = generation_;
        return true;
    }

    // Wake every client so their send loops can exit.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            done_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    FramePtr frame_;
    std::uint64_t generation_ = 0;
    bool done_ = false;
};

class MjpegServer {
public:
    using FrameFn = std::function<bool(cv::Mat&)>;

    // Blocks until @p stop is set, accepting clients and streaming the latest
    // frame to each. @p frameFn fills a BGR cv::Mat and should return false
    // when there is nothing more to send; it is only ever called from the
    // serving thread, so it does not have to be reentrant.
    bool serve(int port, const FrameFn& frameFn, const std::atomic<bool>& stop);

private:
    static bool sendAll(int fd, const char* data, std::size_t len);
    static void handleClient(int fd, FrameBroadcaster& broadcaster,
                             const std::atomic<bool>& stop);
};

inline bool MjpegServer::sendAll(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        const ssize_t sent = ::send(fd, data, len, MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        len -= static_cast<std::size_t>(sent);
    }
    return true;
}

inline bool MjpegServer::serve(int port, const FrameFn& frameFn,
                               const std::atomic<bool>& stop) {
    const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        return false;
    }

    int opt = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd);
        return false;
    }
    if (::listen(listenFd, 16) < 0) {
        ::close(listenFd);
        return false;
    }

    // Non-blocking accept, so the loop can notice `stop` and shut down
    // promptly instead of sitting inside accept().
    const int flags = ::fcntl(listenFd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(listenFd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(listenFd);
        return false;
    }

    FrameBroadcaster broadcaster;
    std::atomic<int> watchers{0};
    std::vector<std::thread> clients;
    std::vector<std::shared_ptr<std::atomic<bool>>> clientDone;

    while (!stop.load()) {
        // ---- accept everyone who is waiting ----
        for (;;) {
            const int client = ::accept(listenFd, nullptr, nullptr);
            if (client < 0) {
                break;
            }
            // A wedged client must not block its thread indefinitely.
            timeval timeout{};
            timeout.tv_sec = 5;
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                         sizeof(timeout));

            watchers.fetch_add(1);
            auto done = std::make_shared<std::atomic<bool>>(false);
            clientDone.push_back(done);
            clients.emplace_back([client, &broadcaster, &stop, &watchers, done] {
                handleClient(client, broadcaster, stop);
                watchers.fetch_sub(1);
                done->store(true);
                ::close(client);
            });
        }

        // ---- reap the clients that have finished ----
        for (std::size_t i = 0; i < clients.size();) {
            if (clientDone[i]->load()) {
                if (clients[i].joinable()) {
                    clients[i].join();
                }
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
                clientDone.erase(clientDone.begin() +
                                 static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }

        if (watchers.load() == 0) {
            // Nobody is watching yet — idle instead of decoding frames.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // ---- produce the next frame ----
        cv::Mat frame;
        if (!frameFn(frame) || frame.empty()) {
            break;
        }
        auto jpeg = std::make_shared<std::vector<unsigned char>>();
        if (!cv::imencode(".jpg", frame, *jpeg, {cv::IMWRITE_JPEG_QUALITY, 70})) {
            break;
        }
        broadcaster.publish(jpeg);
    }

    // Release every client, then wait for their threads to finish.
    broadcaster.shutdown();
    for (auto& t : clients) {
        if (t.joinable()) {
            t.join();
        }
    }
    ::close(listenFd);
    return true;
}

inline void MjpegServer::handleClient(int fd, FrameBroadcaster& broadcaster,
                                      const std::atomic<bool>& stop) {
    // Read just the request line; we only distinguish "/stream" from
    // everything else (served as a small HTML page embedding the live stream).
    char request[4096];
    const ssize_t n = ::recv(fd, request, sizeof(request) - 1, 0);
    if (n <= 0) {
        return;
    }
    request[n] = '\0';
    const bool wantStream = std::strstr(request, "/stream") != nullptr;

    if (!wantStream) {
        static const char kHtml[] =
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n"
            "<!doctype html><html><body style='margin:0;background:#000'>"
            "<img src='/stream' style='width:100%'></body></html>";
        sendAll(fd, kHtml, std::strlen(kHtml));
        return;
    }

    static const char kHeader[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=FRAME\r\n"
        "Cache-Control: no-cache\r\n\r\n";
    if (!sendAll(fd, kHeader, std::strlen(kHeader))) {
        return;
    }

    std::uint64_t lastSeen = 0;
    while (!stop.load()) {
        FrameBroadcaster::FramePtr frame;
        std::uint64_t generation = 0;
        if (!broadcaster.await(lastSeen, frame, generation)) {
            break;
        }
        lastSeen = generation;

        const std::string head =
            "--FRAME\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: " + std::to_string(frame->size()) + "\r\n\r\n";
        if (!sendAll(fd, head.data(), head.size())) {
            break;
        }
        if (!sendAll(fd, reinterpret_cast<const char*>(frame->data()),
                     frame->size())) {
            break;
        }
        if (!sendAll(fd, "\r\n", 2)) {
            break;
        }
    }
}

} // namespace apps
