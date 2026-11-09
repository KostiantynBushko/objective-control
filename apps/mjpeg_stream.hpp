#pragma once

// Minimal MJPEG-over-HTTP streaming server (no external dependencies beyond
// OpenCV's JPEG encoder). Used by the demo app to stream live camera frames to
// a web browser at http://<host>:<port>/ (or /stream for the raw image stream).
//
// Intended only for local/trusted networks — there is no authentication, TLS,
// or security hardening.

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace apps {

class MjpegServer {
public:
    using FrameFn = std::function<bool(cv::Mat&)>;

    // Blocks forever (until the process is interrupted), accepting clients and
    // streaming the latest frame to each. @p frameFn fills a BGR cv::Mat and
    // should return false when there is nothing more to send.
    bool serve(int port, const FrameFn& frameFn, const std::atomic<bool>& stop);

private:
    void handleClient(int fd, const FrameFn& frameFn, const std::atomic<bool>& stop);

    static bool sendAll(int fd, const char* data, std::size_t len) {
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
};

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
    if (::listen(listenFd, 8) < 0) {
        ::close(listenFd);
        return false;
    }

    while (!stop.load()) {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        handleClient(client, frameFn, stop);
        ::close(client);
    }

    ::close(listenFd);
    return true;
}

inline void MjpegServer::handleClient(int fd, const FrameFn& frameFn,
                                      const std::atomic<bool>& stop) {
    // Read just the request line; we only distinguish "/stream" from everything
    // else (served as a small HTML page embedding the live stream).
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

    std::vector<unsigned char> jpeg;
    cv::Mat frame;
    while (!stop.load()) {
        if (!frameFn(frame) || frame.empty()) {
            break;
        }

        jpeg.clear();
        if (!cv::imencode(".jpg", frame, jpeg,
                          {cv::IMWRITE_JPEG_QUALITY, 70})) {
            break;
        }

        const std::string head =
            "--FRAME\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: " + std::to_string(jpeg.size()) + "\r\n\r\n";
        if (!sendAll(fd, head.data(), head.size())) {
            break;
        }
        if (!sendAll(fd, reinterpret_cast<const char*>(jpeg.data()),
                     jpeg.size())) {
            break;
        }
        if (!sendAll(fd, "\r\n", 2)) {
            break;
        }
    }
}

} // namespace apps
