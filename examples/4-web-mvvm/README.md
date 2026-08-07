# Example 4 — Web MVVM via Aria's HTTP adapter

This example demonstrates Aria's `HttpAdapter` exposing a C++ ViewModel
to a browser over a small HTTP/REST/SSE protocol — with optional HTTPS.

## What it shows

* One C++ ViewModel: text greeting + integer counter + reset action.
* The same ViewModel is wired to `HttpAdapter` (could simultaneously
  be bound to `QtAdapter` for a desktop window).
* The browser opens `index.html`, subscribes to state via SSE, and
  drives commands via REST POSTs.
* Bidirectional binding: typing in the input updates the C++ shadow
  state on every keystroke; the server-driven counter ticks update
  the browser without polling.

## Build

The demo builds inside its own isolated tree under `build/flavors/web-demo/`
(the main build mirrors examples via `add_subdirectory` — see the layout map
at the top of `scripts/build.sh`):

```bash
cmake -S . -B build/flavors/web-demo -DARIA_BUILD_HTTP=ON
cmake --build build/flavors/web-demo --target example_4_web_mvvm
```

> First-time configure with `ARIA_HTTP_ENABLE_TLS=ON` (the default)
> builds OpenSSL from source — adds 1-2 minutes to the first build,
> but the binary is fully self-contained afterwards.

## Run (plain HTTP)

```bash
./build/flavors/web-demo/bin/example_4_web_mvvm
```

Then open <http://localhost:19090/> in a browser.

## Run (HTTPS with self-signed cert)

```bash
# Generate a self-signed cert for localhost (one-time):
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout /tmp/aria_key.pem \
    -out /tmp/aria_cert.pem \
    -days 365 \
    -subj "/CN=localhost"

# Run with TLS — third arg is static-root (empty=disabled):
./build/flavors/web-demo/bin/example_4_web_mvvm "" /tmp/aria_cert.pem /tmp/aria_key.pem
```

Then open <https://localhost:19090/> and accept the self-signed cert
warning. For production, use a CA-issued certificate instead.

## Wire-protocol reference

See `modules/adapters/http/include/aria/adapters/http/wire_protocol.hpp`.
