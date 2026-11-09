#pragma once

// Self-contained HTTP client for "network webcams":
//   * MJPEG-over-HTTP streams (Content-Type: multipart/x-mixed-replace)
//   * Snapshot URLs (Content-Type: image/jpeg) polled once per read()
//
// Decodes JPEG with OpenCV's imgcodecs, so no FFmpeg/GStreamer is required for
// the HTTP path. TLS (https://) is not supported; RTSP is handled elsewhere.

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace oc::detail {

class HttpMjpegClient {
public:
    ~HttpMjpegClient() { close(); }

    // Opens the stream/snapshot at @p url (http://host[:port][/path]).
    // Returns false if the URL is invalid, the connection fails, or TLS is
    // requested (unsupported).
    bool open(const std::string& url) {
        close();
        if (!parseUrl(url)) {
            return false;
        }
        if (scheme_ != "http") {
            return false; // only plain HTTP (rtsp handled elsewhere, no TLS)
        }
        if (!connect()) {
            return false;
        }
        if (!sendRequest()) {
            close();
            return false;
        }
        if (!readResponseHeaders()) {
            close();
            return false;
        }
        if (mode_ == Mode::None) {
            close();
            return false;
        }
        if (mode_ == Mode::Snapshot) {
            // The first frame body is already buffered; decode it on read().
            return true;
        }
        return true;
    }

    // Fills @p out with the next decoded BGR frame. Returns false on EOF or
    // error. For snapshot mode this issues a fresh request and returns one
    // frame per call.
    bool read(cv::Mat& out) {
        if (mode_ == Mode::Multipart) {
            std::vector<std::uint8_t> jpeg;
            if (!nextPart(jpeg)) {
                return false;
            }
            return decode(jpeg, out);
        }
        if (mode_ == Mode::Snapshot) {
            if (!firstSnapshotDecoded_) {
                firstSnapshotDecoded_ = true;
                if (!snapshot_.empty()) {
                    return decode(snapshot_, out);
                }
            }
            // Re-request a fresh snapshot over a new connection.
            close();
            if (!connect() || !sendRequest() || !readResponseHeaders() ||
                mode_ != Mode::Snapshot) {
                return false;
            }
            return decode(snapshot_, out);
        }
        return false;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        pos_ = len_ = 0;
        buf_.clear();
    }

private:
    enum class Mode { None, Multipart, Snapshot };

    bool parseUrl(const std::string& url) {
        const auto schemeEnd = url.find("://");
        if (schemeEnd == std::string::npos) {
            return false;
        }
        scheme_ = url.substr(0, schemeEnd);
        const std::string rest = url.substr(schemeEnd + 3);
        const auto slash = rest.find('/');
        const std::string hostport = (slash == std::string::npos)
                                         ? rest
                                         : rest.substr(0, slash);
        path_ = (slash == std::string::npos) ? "/" : rest.substr(slash);

        const auto colon = hostport.rfind(':');
        if (colon != std::string::npos && hostport.find(':') == colon) {
            host_ = hostport.substr(0, colon);
            const std::string portPart = hostport.substr(colon + 1);
            port_ = portPart.empty() ? 80 : std::atoi(portPart.c_str());
            if (port_ <= 0 || port_ > 65535) {
                return false;
            }
        } else {
            host_ = hostport;
            port_ = (scheme_ == "https") ? 443 : 80;
        }
        return !host_.empty();
    }

    bool connect() {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        const std::string portStr = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &result) != 0) {
            return false;
        }
        fd_ = ::socket(result->ai_family, result->ai_socktype,
                       result->ai_protocol);
        if (fd_ < 0) {
            freeaddrinfo(result);
            return false;
        }
        const int ok = ::connect(fd_, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);
        if (ok != 0) {
            close();
            return false;
        }
        pos_ = len_ = 0;
        buf_.clear();
        return true;
    }

    bool sendRequest() {
        const std::string req =
            "GET " + path_ + " HTTP/1.0\r\n"
            "Host: " + host_ + "\r\n"
            "User-Agent: objective-control\r\n"
            "Accept: */*\r\n"
            "\r\n";
        return ::send(fd_, req.data(), req.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(req.size());
    }

    // Lowercases @p s in place.
    static void lower(std::string& s) {
        for (auto& c : s) {
            c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
    }

    bool readResponseHeaders() {
        std::string line;
        // Status line (ignore code for now).
        if (!getLine(line) || line.empty()) {
            return false;
        }

        mode_ = Mode::None;
        snapshot_.clear();
        int contentLength = -1;

        for (;;) {
            if (!getLine(line)) {
                return false;
            }
            if (line.empty()) {
                break; // end of headers
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            lower(name);
            while (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }
            if (name == "content-type") {
                if (value.find("multipart/x-mixed-replace") !=
                    std::string::npos) {
                    mode_ = Mode::Multipart;
                    boundary_ = extractBoundary(value);
                } else if (value.find("image/jpeg") != std::string::npos) {
                    mode_ = Mode::Snapshot;
                }
            } else if (name == "content-length") {
                contentLength = std::atoi(value.c_str());
            }
        }

        if (mode_ == Mode::Snapshot && contentLength > 0) {
            snapshot_.resize(static_cast<std::size_t>(contentLength));
            if (!readExact(snapshot_.data(), snapshot_.size())) {
                return false;
            }
        }
        firstSnapshotDecoded_ = false;
        return mode_ != Mode::None;
    }

    static std::string extractBoundary(const std::string& contentType) {
        const std::string key = "boundary=";
        const auto pos = contentType.find(key);
        if (pos == std::string::npos) {
            return "";
        }
        std::string b = contentType.substr(pos + key.size());
        // Strip a possible trailing ";..." segment.
        const auto semi = b.find(';');
        if (semi != std::string::npos) {
            b = b.substr(0, semi);
        }
        return b;
    }

    // Reads the JPEG payload of the next multipart part.
    bool nextPart(std::vector<std::uint8_t>& jpeg) {
        // Advance to the next "--boundary" line.
        std::string line;
        for (;;) {
            if (!getLine(line)) {
                return false;
            }
            if (line.rfind("--" + boundary_, 0) == 0) {
                if (line == "--" + boundary_ + "--") {
                    return false; // terminating boundary
                }
                break;
            }
        }
        // Read part headers until the blank line, capturing Content-Length.
        int contentLength = -1;
        for (;;) {
            if (!getLine(line)) {
                return false;
            }
            if (line.empty()) {
                break;
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            std::string name = line.substr(0, colon);
            lower(name);
            if (name == "content-length") {
                contentLength = std::atoi(line.c_str() + colon + 1);
            }
        }
        if (contentLength <= 0) {
            return false;
        }
        jpeg.resize(static_cast<std::size_t>(contentLength));
        return readExact(jpeg.data(), jpeg.size());
    }

    static bool decode(const std::vector<std::uint8_t>& jpeg, cv::Mat& out) {
        if (jpeg.empty()) {
            return false;
        }
        const cv::Mat encoded(1, static_cast<int>(jpeg.size()), CV_8UC1,
                              const_cast<std::uint8_t*>(jpeg.data()));
        out = cv::imdecode(encoded, cv::IMREAD_COLOR);
        return !out.empty();
    }

    // ---- buffered byte reader over the socket ----

    bool fill() {
        if (pos_ < len_) {
            return true;
        }
        buf_.resize(65536);
        const ssize_t n = ::recv(fd_, buf_.data(), buf_.size(), 0);
        if (n <= 0) {
            return false;
        }
        pos_ = 0;
        len_ = static_cast<std::size_t>(n);
        return true;
    }

    bool getLine(std::string& out) {
        out.clear();
        for (;;) {
            for (std::size_t i = pos_; i < len_; ++i) {
                if (buf_[i] == '\n') {
                    out.append(&buf_[pos_], i - pos_);
                    pos_ = i + 1;
                    if (!out.empty() && out.back() == '\r') {
                        out.pop_back();
                    }
                    return true;
                }
            }
            // The buffer may not have been allocated yet on the very first
            // call (fill() is what resizes it), so guard the append instead
            // of indexing an empty vector.
            if (len_ > pos_) {
                out.append(&buf_[pos_], len_ - pos_);
            }
            pos_ = len_;
            if (!fill()) {
                return !out.empty();
            }
        }
    }

    bool readExact(void* p, std::size_t n) {
        auto* cp = static_cast<char*>(p);
        while (n > 0) {
            if (pos_ < len_) {
                const std::size_t avail = len_ - pos_;
                const std::size_t take = std::min(avail, n);
                std::memcpy(cp, &buf_[pos_], take);
                pos_ += take;
                cp += take;
                n -= take;
            } else if (!fill()) {
                return false;
            }
        }
        return true;
    }

    std::string scheme_;
    std::string host_;
    int port_ = 80;
    std::string path_;
    std::string boundary_;
    Mode mode_ = Mode::None;
    std::vector<std::uint8_t> snapshot_;
    bool firstSnapshotDecoded_ = false;
    int fd_ = -1;
    std::vector<char> buf_;
    std::size_t pos_ = 0;
    std::size_t len_ = 0;
};

} // namespace oc::detail
