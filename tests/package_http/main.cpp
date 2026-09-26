#include <aria/adapters/http/http_adapter.hpp>

#include <cstdint>
#include <iostream>
#include <string>

// Minimal standalone HTTP/1.1 client for the installed-SDK smoke test: no
// Aria test helpers, no Mira types — a raw loopback exchange over the
// public adapter surface, plus the build-definition contract checks.

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

bool get_health(std::uint16_t port, std::string& body) {
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    SOCKET handle = ::socket(AF_INET, SOCK_STREAM, 0);
#else
    int handle = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (handle < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
#if defined(_WIN32)
    InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
#else
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) return false;
#endif
    if (::connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
#if defined(_WIN32)
        closesocket(handle);
#else
        ::close(handle);
#endif
        return false;
    }
    const char* request = "GET /aria/health HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Connection: close\r\n\r\n";
    if (::send(handle, request, static_cast<int>(std::string(request).size()), 0) < 0) {
#if defined(_WIN32)
        closesocket(handle);
#else
        ::close(handle);
#endif
        return false;
    }
    body.clear();
    char buffer[1024];
    for (;;) {
#if defined(_WIN32)
        const int got = ::recv(handle, buffer, sizeof(buffer), 0);
#else
        const ssize_t got = ::recv(handle, buffer, sizeof(buffer), 0);
#endif
        if (got <= 0) break;
        body.append(buffer, static_cast<std::size_t>(got));
    }
#if defined(_WIN32)
    closesocket(handle);
    WSACleanup();
#else
    ::close(handle);
#endif
    return true;
}

}  // namespace

int main() {
    aria::adapters::http::HttpAdapterConfig config;
    config.port = 0;
    config.worker_threads = 2;
    aria::adapters::http::HttpAdapter adapter(config);
    if (!adapter.start()) {
        std::cerr << "adapter failed to start\n";
        return 1;
    }
    std::string wire;
    if (!get_health(adapter.actual_port(), wire)) {
        std::cerr << "loopback request failed\n";
        return 2;
    }
    const auto body_at = wire.find("\r\n\r\n");
    if (body_at == std::string::npos) {
        std::cerr << "malformed response\n";
        return 3;
    }
    // Plain substring checks keep this consumer dependency-free: the health
    // payload is `{"ok":true,"protocol":2}` from a fixed server version.
    const std::string payload = wire.substr(body_at + 4);
    if (payload.find("\"ok\":true") == std::string::npos ||
        payload.find("\"protocol\":2") == std::string::npos) {
        std::cerr << "unexpected health payload: " << payload << '\n';
        return 4;
    }
    std::cout << "ARIA_HTTP_PACKAGE_PASS tls="
#if defined(ARIA_HTTP_HAS_TLS)
              << 1
#else
              << 0
#endif
              << '\n';
    adapter.stop();
    return 0;
}
