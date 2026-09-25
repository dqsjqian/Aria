#pragma once
/// @file test_http_client.hpp
/// @brief Minimal blocking HTTP/1.1 test client for adapter wire tests.
///
/// Deliberately independent of Continuo's own HTTP client: exercising the
/// adapter through plain sockets keeps the test honest about the wire. It
/// speaks just enough HTTP for the adapter's surface — request lines,
/// Content-Length and chunked responses, and incremental reads for SSE.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <cstring>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace test_http {

inline void close_socket(SocketHandle handle) {
    if (handle == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(handle);
#else
    ::close(handle);
#endif
}

/// Process-wide WSA startup (no-op elsewhere).
struct SocketSystem {
    SocketSystem() {
#if defined(_WIN32)
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
    }
};

inline void set_receive_timeout(SocketHandle handle, int seconds) {
#if defined(_WIN32)
    const DWORD milliseconds = static_cast<DWORD>(seconds) * 1000;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
#else
    timeval timeout{};
    timeout.tv_sec = seconds;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

struct Response {
    int status = 0;
    std::string headers;  // raw header block, for targeted inspections
    std::string body;
};

/// One TCP connection; requests are written and responses read in order,
/// so keep-alive reuse is a natural part of the test vocabulary.
class Client {
public:
    explicit Client(std::uint16_t port) {
        static SocketSystem system;
        (void)system;
        handle_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (handle_ == kInvalidSocket) return;
        set_receive_timeout(handle_, 5);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
#if defined(_WIN32)
        InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
#else
        REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
#endif
        if (::connect(handle_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close_socket(handle_);
            handle_ = kInvalidSocket;
        }
    }

    ~Client() { stop(); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client(Client&& other) noexcept
        : handle_(std::exchange(other.handle_, kInvalidSocket)),
          buffer_(std::move(other.buffer_)) {}
    Client& operator=(Client&& other) noexcept {
        if (this != &other) {
            stop();
            handle_ = std::exchange(other.handle_, kInvalidSocket);
            buffer_ = std::move(other.buffer_);
        }
        return *this;
    }

    /// Unblock a concurrent reader and close. Safe to call twice.
    void stop() {
        if (handle_ == kInvalidSocket) return;
#if defined(_WIN32)
        shutdown(handle_, SD_BOTH);
        closesocket(handle_);
#else
        ::shutdown(handle_, SHUT_RDWR);
        ::close(handle_);
#endif
        handle_ = kInvalidSocket;
    }

    [[nodiscard]] bool connected() const noexcept { return handle_ != kInvalidSocket; }

    /// Write a complete request. Keep-alive: call again on the same client.
    void send(std::string_view method, std::string_view target,
              std::string_view body = {}, std::string_view content_type = {}) {
        std::string request;
        request.reserve(128 + body.size());
        request += method;
        request += ' ';
        request += target;
        request += " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        if (!body.empty()) {
            request += "Content-Type: ";
            request += content_type;
            request += "\r\nContent-Length: ";
            request += std::to_string(body.size());
            request += "\r\n";
        }
        request += "Connection: keep-alive\r\n\r\n";
        request += body;
        write_all(request);
    }

    /// Read one response with a hard wall-clock deadline. HEAD responses
    /// carry Content-Length but no body — like the wire says.
    [[nodiscard]] Response read_response(std::chrono::seconds budget = std::chrono::seconds(5),
                                         bool head_request = false) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        Response out;
        std::string head = read_until("\r\n\r\n", deadline);
        out.headers = head;
        const std::size_t status_end = head.find("\r\n");
        REQUIRE(status_end != std::string::npos);
        {
            // "HTTP/1.1 200 OK" → 200
            const std::string_view line(head.data(), status_end);
            const auto first_space = line.find(' ');
            REQUIRE(first_space != std::string_view::npos);
            out.status = std::stoi(std::string(line.substr(first_space + 1, 3)));
        }
        const std::string lower_head = lowercase(head);
        const bool no_body = out.status == 204 || out.status == 304 || out.status < 200;
        if (lowercase_contains(lower_head, "transfer-encoding: chunked")) {
            std::string body;
            for (;;) {
                std::string size_line = read_until("\r\n", deadline);
                const unsigned long size =
                    std::stoul(size_line.substr(0, size_line.find(';')), nullptr, 16);
                if (size == 0) {
                    consume(2, deadline);  // final CRLF
                    break;
                }
                const std::string chunk = consume(static_cast<std::size_t>(size), deadline);
                body += chunk;
                consume(2, deadline);  // chunk CRLF
            }
            out.body = std::move(body);
        } else if (auto length = header_value(head, "Content-Length"); !length.empty()) {
            if (!head_request) {
                out.body = consume(static_cast<std::size_t>(std::stoul(length)), deadline);
            }
        } else if (!no_body) {
            out.body = read_until_eof(deadline);
        }
        return out;
    }

    /// Convenience: one request + one response on this connection.
    [[nodiscard]] Response request(std::string_view method, std::string_view target,
                                   std::string_view body = {},
                                   std::string_view content_type = {},
                                   std::chrono::seconds budget = std::chrono::seconds(5)) {
        send(method, target, body, content_type);
        return read_response(budget, method == "HEAD");
    }

    [[nodiscard]] Response get(std::string_view target,
                               std::chrono::seconds budget = std::chrono::seconds(5)) {
        return request("GET", target, {}, {}, budget);
    }

    [[nodiscard]] Response post(std::string_view target, std::string_view body,
                                std::chrono::seconds budget = std::chrono::seconds(5)) {
        return request("POST", target, body, "application/json", budget);
    }

    /// Streaming read for SSE: `sink` gets every raw piece as it arrives and
    /// returns false to stop. Returns when the sink stops or the budget ends.
    void stream(std::string_view target,
                const std::function<bool(std::string_view)>& sink,
                std::chrono::seconds budget = std::chrono::seconds(5)) {
        send("GET", target);
        const auto deadline = std::chrono::steady_clock::now() + budget;
        // Consume the head first; the tests only need the events. Bytes read
        // past the head are already in the internal buffer — flush them to
        // the sink before reading more, or a fast server's frames would sit
        // there unseen.
        static_cast<void>(read_until("\r\n\r\n", deadline));
        if (!buffer_.empty()) {
            if (!sink(std::exchange(buffer_, {}))) return;
        }
        for (;;) {
            if (std::chrono::steady_clock::now() > deadline) return;
            char chunk[1024];
            const std::size_t got = read_some(chunk, sizeof(chunk));
            if (got == 0) return;
            if (!sink(std::string_view{chunk, got})) return;
        }
    }

    SocketHandle raw() const noexcept { return handle_; }

private:
    static std::string lowercase(std::string_view text) {
        std::string out(text);
        for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    static bool lowercase_contains(const std::string& hay, std::string_view needle) {
        return hay.find(needle) != std::string::npos;
    }

    static std::string header_value(const std::string& head, std::string_view name) {
        std::string lower = lowercase(head);
        std::string needle = lowercase(name);
        const std::size_t at = lower.find(needle + ":");
        if (at == std::string::npos) return {};
        const std::size_t value_start = head.find_first_not_of(" \t", at + needle.size() + 1);
        const std::size_t line_end = head.find("\r\n", at);
        if (value_start == std::string::npos || line_end == std::string::npos ||
            value_start > line_end) {
            return {};
        }
        return head.substr(value_start, line_end - value_start);
    }

    void write_all(std::string_view data) {
        REQUIRE(connected());
        std::size_t sent = 0;
        while (sent < data.size()) {
#if defined(_WIN32)
            const int n = ::send(handle_, data.data() + sent,
                                 static_cast<int>(data.size() - sent), 0);
#else
            const ssize_t n = ::send(handle_, data.data() + sent, data.size() - sent, 0);
#endif
            REQUIRE(n > 0);
            sent += static_cast<std::size_t>(n);
        }
    }

    std::size_t read_some(void* destination, std::size_t size) {
#if defined(_WIN32)
        const int n = ::recv(handle_, reinterpret_cast<char*>(destination),
                             static_cast<int>(size), 0);
#else
        const ssize_t n = ::recv(handle_, destination, size, 0);
#endif
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

    std::string read_until(std::string_view delimiter, const std::chrono::steady_clock::time_point& deadline) {
        std::string out;
        for (;;) {
            const std::size_t at = buffer_.find(delimiter);
            if (at != std::string::npos) {
                out += buffer_.substr(0, at + delimiter.size());
                buffer_.erase(0, at + delimiter.size());
                return out;
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            char chunk[1024];
            const std::size_t got = read_some(chunk, sizeof(chunk));
            if (got == 0) {
                // Peer hung up mid-message. Surface as an unparseable
                // response instead of aborting: some tests deliberately
                // tear the server down mid-request.
                MESSAGE("test_http: peer closed mid-response");
                out += buffer_;
                buffer_.clear();
                out += "\x01peer-closed";
                return out;
            }
            buffer_.append(chunk, got);
        }
    }

    std::string consume(std::size_t size, const std::chrono::steady_clock::time_point& deadline) {
        std::string out;
        while (out.size() < size) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            if (!buffer_.empty()) {
                const std::size_t take = std::min(size - out.size(), buffer_.size());
                out += buffer_.substr(0, take);
                buffer_.erase(0, take);
                continue;
            }
            char chunk[1024];
            const std::size_t got = read_some(chunk, sizeof(chunk));
            REQUIRE(got > 0);
            out.append(chunk, got);
        }
        return out;
    }

    std::string read_until_eof(const std::chrono::steady_clock::time_point& deadline) {
        std::string out = std::exchange(buffer_, {});
        for (;;) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            char chunk[1024];
            const std::size_t got = read_some(chunk, sizeof(chunk));
            if (got == 0) return out;
            out.append(chunk, got);
        }
    }

    SocketHandle handle_ = kInvalidSocket;
    std::string buffer_;
};

/// SSE accumulator mirroring the wire protocol's frame discipline.
class Stream {
public:
    explicit Stream(std::uint16_t port) {
        client_ = std::make_unique<Client>(port);
        connected_ = client_->connected();
        thread_ = std::thread([this] {
            if (!connected_) return;
            client_->stream("/aria/stream", [this](std::string_view piece) {
                {
                    std::lock_guard lock(mu_);
                    frames_.append(piece);
                }
                cv_.notify_all();
                return true;
            });
        });
    }

    ~Stream() {
        client_->stop();  // unblocks the reader; the 5s deadline is the backstop
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] bool connected() const { return connected_; }

    [[nodiscard]] bool wait_for(std::string_view text) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, std::chrono::seconds(4),
                            [&] { return frames_.find(text) != std::string::npos; });
    }

    [[nodiscard]] std::vector<nlohmann::json> events() {
        std::lock_guard lock(mu_);
        std::vector<nlohmann::json> out;
        std::size_t start = 0;
        while ((start = frames_.find("data: ", start)) != std::string::npos) {
            const std::size_t end = frames_.find("\n\n", start);
            if (end == std::string::npos) break;
            out.push_back(nlohmann::json::parse(frames_.substr(start + 6, end - start - 6)));
            start = end + 2;
        }
        return out;
    }

private:
    std::unique_ptr<Client> client_;
    std::thread thread_;
    bool connected_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
    std::string frames_;
};

}  // namespace test_http
