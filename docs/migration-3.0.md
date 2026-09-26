# Migrating to Aria 3.0

Aria 3.0 raises the language baseline to C++23, rebuilds the HTTP adapter on
Mira, and replaces the vendored third-party model with hash-pinned
downloads. This page lists everything that can break an existing integration.

## C++23 is required

Every Aria module — and therefore every consumer — now compiles as C++23.
`CMAKE_CXX_STANDARD` defaults to 23 and the configure step rejects anything
lower.

| Toolchain | Minimum |
|---|---|
| GCC | 13 |
| Clang / AppleClang | 18 / 21 |
| MSVC | Visual Studio 2022 (v143) |

Action: bump your project's `CMAKE_CXX_STANDARD` to 23 (or remove the
override — Aria's default now applies).

## HTTP adapter (aria::http)

### `native_server()` is gone

The escape hatch that exposed the underlying `httplib::Server&` was coupled
to the previous backend and is removed. Custom REST routes map to
`register_command()`:

```cpp
// Before (Aria 2.x)
adapter.native_server().Get("/custom", [](const httplib::Request&,
                                          httplib::Response& res) {
    res.set_content("ok", "text/plain");
});

// After (Aria 3.x)
adapter.register_command("view-id", "custom-route",
    [](std::string_view args_json) {
        return std::string("ok");
    });
```

### Behaviour notes

- The wire protocol is unchanged (REST + SSE, protocol version 2). Browsers
  and the bundled Web SDK need no changes.
- SSE clients no longer consume worker threads, so `max_sse_clients` is the
  single admission cap; `0` means a default of 64 instead of "bounded only by
  the worker count".
- Static mounts are served by the adapter: `index.html` default, extension
  based Content-Type, traversal refusal. HTTP ranges and directory listings
  are not supported.
- Bind addresses must be numeric; `"localhost"` maps to the IPv4 loopback.
- TLS still requires `ARIA_HTTP_ENABLE_TLS=ON` plus OpenSSL 3+ (OpenSSL 4.0
  verified). `tls_ca_file` now enforces mandatory client verification through
  Mira; there is no insecure bypass.

## Dependencies: pinned downloads instead of third_party

`third_party/` (cpp-httplib, doctest, nlohmann_json) and the OpenSSL git
submodule are removed. Dependencies are downloaded at configure time and
verified against SHA256 hashes pinned in the CMake files
(`cmake/ariaFetchPinned.cmake`):

| Dependency | Version | Source |
|---|---|---|
| Mira | 0.1.0 | GitHub release asset |
| nlohmann/json | 3.12.0 | `json.tar.xz` release asset |
| doctest | 2.5.3 | raw header at tag |
| OpenSSL (TLS builds) | 4.0.2 | GitHub release asset |

- First configure needs network access; the cache lives under `build/_deps`
  and is shared by every build flavor.
- Offline / patched builds: `-DARIA_PIN_MIRA_SOURCE_DIR=/path/to/Mira`
  (likewise `ARIA_PIN_OPENSSL_SOURCE_DIR`, …) uses a local tree as-is.
- CI: pre-warm the cache or accept the configure-time download; there is no
  submodule step anymore.
- Static `aria_http` additionally requires the Mira package at consumer
  configure time (`find_package(aria)` pulls it in via
  `find_dependency(Mira)`). Shared builds absorb Mira into the
  `aria_http` dylib and need nothing extra.
