#pragma once
/// @file http_config.hpp
/// @brief Configuration for the HTTP adapter.

#include "aria/abi/export.hpp"

#include <cstdint>
#include <string>

namespace aria::adapters::http {

/// Configuration for HttpAdapter. All fields have safe defaults; the
/// canonical "just works" instance is `HttpAdapterConfig{}`.
struct ARIA_HTTP_API HttpAdapterConfig {
    /// Bind address. Default "127.0.0.1" — localhost-only for safety.
    /// Use "0.0.0.0" to expose on all interfaces (only do this if you
    /// have authentication / a reverse proxy in front).
    std::string host{"127.0.0.1"};

    /// TCP port. 0 lets the OS pick — read it back via
    /// HttpAdapter::actual_port() after start().
    std::uint16_t port{9090};

    /// Path prefix for REST endpoints. Default "/aria". The SSE stream
    /// is mounted at `<api_prefix>/stream`.
    std::string api_prefix{"/aria"};

    /// Optional static-file root. When non-empty, the adapter serves
    /// HTML/JS/CSS at "/" — convenient for shipping the bundled web SDK
    /// + an index.html in the same process.
    std::string static_root{};

    /// Worker thread count for the HTTP server.
    /// 0 = std::thread::hardware_concurrency().
    int worker_threads{0};

    /// Heartbeat interval (seconds) for SSE keep-alive pings.
    /// Most reverse proxies idle-close at 30~60s; keep below.
    int heartbeat_sec{25};

    /// Maximum number of concurrent SSE clients. Excess connections
    /// receive 503 until existing ones drop. 0 = unlimited.
    int max_sse_clients{64};

    /// Enable CORS headers (Access-Control-Allow-Origin: *).
    /// Default false. Enable only for development; for production,
    /// set up a reverse proxy with proper CORS policy.
    bool enable_cors{false};

    // ── TLS (HTTPS) ──────────────────────────────────────────────────
    //
    // When `tls_cert_file` and `tls_key_file` are both non-empty AND
    // the adapter was built with `ARIA_HTTP_ENABLE_TLS=ON` (which links
    // OpenSSL statically — see cmake/BuildOpenSSL.cmake), the server
    // serves HTTPS instead of plain HTTP. Both endpoints (REST and
    // SSE) move to TLS.
    //
    // Set BOTH cert and key paths to PEM files. Optionally set
    // `tls_ca_file` to require client certificate verification (mTLS).
    // Set `tls_min_version` to enforce a minimum TLS version
    // ("1.2" or "1.3"; default is "1.2" — TLS 1.0/1.1 are always
    // refused).
    //
    // For local development you can quickly mint a self-signed cert:
    //
    //   openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem
    //       -out cert.pem -days 365 -subj "/CN=localhost"
    //
    // For production, use a real CA-issued cert (Let's Encrypt etc.).

    /// Server certificate (PEM). Empty = no TLS (plain HTTP).
    std::string tls_cert_file{};

    /// Server private key (PEM). Empty = no TLS.
    std::string tls_key_file{};

    /// Optional CA bundle for verifying client certificates (mTLS).
    /// Empty = no client cert required.
    std::string tls_ca_file{};

    /// Minimum TLS version: "1.2" (default, broad compat) or "1.3"
    /// (modern, recommended for new deployments). TLS 1.0/1.1 are
    /// never accepted regardless of this setting.
    std::string tls_min_version{"1.2"};
};

}  // namespace aria::adapters::http
