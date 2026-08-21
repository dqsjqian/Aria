# RFC 0001 — HTTP/REST/SSE Adapter

| Field | Value |
|-------|-------|
| Status | **Accepted** |
| Author | dqsjqian |
| Created | 2026-06-08 |
| Module | `modules/adapters/http/` |

## Summary

Add a new platform adapter — **`aria::http::HttpAdapter`** — that
exposes Aria ViewModels to a remote browser via a small HTTP/1.1 +
REST + Server-Sent-Events protocol. The adapter is the "Web sibling"
of `QtAdapter` / `AppKitAdapter` / `UIKitAdapter`: same `IView` and
`IViewAdapter` contract, different transport.

## Motivation

Aria today has three adapters that all bind ViewModels to *native UI
toolkits in the same process* (Qt6, AppKit, UIKit). For two real-world
deployment shapes the framework currently has no answer:

1. **Desktop apps that want a web UI on the side**
   (admin panel, debug dashboard, remote control). Forcing every such
   app to spin up Flask + pybind11 or write a custom REST layer is
   unergonomic and fragments the binding model.

2. **Headless services that want a thin web frontend**
   (IoT gateways, local toolchains, dev-time debugging UIs). These have
   no native UI at all, and bringing in Qt just to host a window is
   the wrong shape.

The `WASM` adapter that's been on the roadmap also serves "Web", but
it solves a different problem (compile C++ into the browser sandbox)
and inherits hard constraints (CORS, no native sockets, no thread
preemption, no filesystem). It is **not a substitute** — for any
business that needs unrestricted network/filesystem/threads, an
HTTP-adapter-hosted web UI is the right shape, not WASM.

## Goals

* **Drop-in IViewAdapter implementation.** Same `IView`/`IViewAdapter`
  contract as Qt/AppKit/UIKit. ViewModels written against the
  framework see no difference; `BindingEngine` works unchanged.
* **No new external build dependency.** The server side reuses the
  vendored single-header **cpp-httplib** (HTTP/1.1 + SSE) and
  **nlohmann::json** — both already committed under `third_party/`, so
  enabling the adapter pulls in no submodule and no package-manager
  step. aria itself owns the wire protocol, view registry, subscription
  dispatch and SSE fan-out in a single `.cpp`. (An earlier draft tried
  to hand-roll the HTTP wire and a JSON parser; see "Why these
  dependencies and not others" below for why that was abandoned.)
* **Vanilla-JS browser SDK.** A ~200 LOC pure-ESM module
  (`web-sdk/aria_client.js`) that works in any modern browser without
  a build step. Frameworks (React/Vue/Svelte) wrap it trivially.
* **Reasonable defaults; safe-by-default.**
  Bind to localhost; rate-limit SSE clients; CORS off; no auth (rely
  on network boundary or a reverse proxy).
* **Bidirectional**: server-side `Property<T>` changes push to the
  browser via SSE; browser-side updates land back as `IViewAdapter`
  callback invocations the same way Qt's `editingFinished` does.

## Non-goals

* Authentication / authorisation. Consumer's job.
* Multi-user session isolation. Each `HttpAdapter` instance is single-
  scope; deploy one per user/tenant if needed.
* WebSocket. SSE-only in v1; v2 may add WS as preferred transport.
* Binary payloads. JSON only in v1.
* Reconnection / replay log. SSE auto-reconnects but state may be
  stale until the next push. (See Future Work.)

## Design

### Class shape

```cpp
namespace aria::adapters::http {

struct HttpAdapterConfig { /* host, port, api_prefix, ... */ };

class HttpView : public binding::IView {
    HttpView(std::string id, std::string kind);
    const std::string& id() const;       // routing key
    std::string_view kind() const;       // "text"/"bool"/"int"/...
};

class HttpAdapter : public binding::IViewAdapter {
public:
    explicit HttpAdapter(HttpAdapterConfig = {});
    bool start();
    void stop();
    HttpView& register_view(std::string id, std::string kind);
    HttpView* find_view(std::string_view id) noexcept;
    void unregister_view(std::string_view id);
    // Plus all IViewAdapter methods.
    using CommandHandler = std::function<std::string(std::string_view)>;
    void register_command(std::string_view view_id,
                          std::string_view command_name,
                          CommandHandler handler);
};
}  // namespace
```

### Wire protocol v1

All payloads are JSON, UTF-8.

| Method | Path                | Purpose |
|--------|---------------------|---------|
| GET    | /aria/health        | Readiness probe |
| GET    | /aria/views         | Enumerate views |
| GET    | /aria/state?view=X  | Snapshot one view |
| POST   | /aria/state         | Update a view |
| POST   | /aria/click         | Fire a click event |
| POST   | /aria/command       | Invoke a custom command |
| GET    | /aria/stream        | SSE subscribe |

Server → Client SSE frames (`data:` lines) carry one JSON object per
frame, of these shapes:

```
{"type":"hello","platform":"http","protocol":1}
{"type":"state","view":"<id>","field":"text|bool|int|double","value":<v>}
{"type":"event","view":"<id>","field":"click"}
{"type":"visibility","view":"<id>","value":true|false}
{"type":"enabled","view":"<id>","value":true|false}
{"type":"ping"}
{"type":"error","message":"<text>"}
```

Servers MUST tolerate unknown fields in client requests; clients MUST
tolerate unknown `type` values. This gives both ends room to evolve
without breaking older deployments.

### Threading model

| Operation | Thread |
|-----------|--------|
| `register_view` / `unregister_view` / `start` / `stop` | Any (locked) |
| `set_text` / `set_bool` / ... (server → client) | Any; non-blocking enqueue |
| `get_text` / ... | Any; O(log N) under shared mutex |
| Inbound HTTP request handling | Server worker pool |
| User callbacks (`on_text_changed`, `on_click`, ...) | Server worker pool |

If a callback needs to marshal to a UI/main thread, the consumer wraps
it with their own dispatcher — same contract as Qt/AppKit adapters.

### Lifetime

* `HttpView` is owned by the adapter's view registry.
* Destroying the adapter (or calling `unregister_view`) fires
  `IView::on_destroy` so any `BindingEngine` subscription wired to
  the view is released cleanly.
* `Subscription` returned from `on_text_changed` / `on_click` etc.
  uses the unified `aria::Subscription` RAII handle.

### Security

The default config binds to `127.0.0.1` and disables CORS. Any
deployment that exposes the adapter beyond localhost MUST run a
reverse proxy that adds: TLS termination, authentication, CORS policy,
rate limiting.

## Implementation notes

### Dependencies (all vendored — zero system-package requirements)

| Library | Author | License | Purpose | Vendored at |
|---------|--------|---------|---------|-------------|
| `cpp-httplib` | Yuji Hirose | MIT | HTTP/1.1 + HTTPS server, routing, chunked SSE streams, worker pool | `third_party/cpp-httplib/httplib.h` |
| `nlohmann::json` | Niels Lohmann | MIT | JSON encode/decode | `third_party/nlohmann_json/include/nlohmann/json.hpp` |
| `OpenSSL 3.5.x` | OpenSSL Project | Apache-2.0 | TLS 1.2 / 1.3 (when `ARIA_HTTP_ENABLE_TLS=ON`) | `third_party/openssl/` (built from source via `cmake/BuildOpenSSL.cmake`) |

All three are widely-used, battle-tested, openly-licensed. They are
**vendored** — no network fetch at build time, no system-package
dependency, no submodule. Stripping any one out is a single directory
delete.

OpenSSL is built from source as a static library by an `ExternalProject`
step the first time you configure with `ARIA_HTTP_ENABLE_TLS=ON` (the
default). This adds ~1-2 minutes to the first build, but the result is
fully self-contained: the resulting `libaria_http` has no runtime
dependency on the user's system OpenSSL. Cross-compilation targets
(macOS arm64/x86_64, Windows MSVC/MinGW, Linux x86_64/aarch64,
Android ARM/x86) are all selected by the build script automatically.

### TLS configuration

Set `HttpAdapterConfig::tls_cert_file` and `tls_key_file` to PEM paths
to enable HTTPS. Optional fields:

* `tls_ca_file` — when set, requires client certificate verification (mTLS).
* `tls_min_version` — `"1.2"` (default) or `"1.3"`. TLS 1.0/1.1 are
  always refused regardless of this setting.

For local development:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout key.pem -out cert.pem -days 365 \
    -subj "/CN=localhost"
```

For production: use a real CA-issued certificate (Let's Encrypt etc.)
or a managed-load-balancer-terminated TLS in front of the adapter.

### Why these dependencies and not others

The previous drafts of this RFC went through two false starts that
are worth recording so future maintainers don't repeat them:

1. **Hand-rolled JSON parser** — initial draft ~200 LOC, missed nested
   objects, UTF-8 surrogate pairs, arrays. Replaced with
   `nlohmann::json`.
2. **Hand-rolled HTTP/1.1 + SSE wire** — second draft ~600 LOC, worked
   for happy-path GET/POST but reinventing chunked encoding /
   keep-alive / graceful shutdown was multiple weeks of work that
   `cpp-httplib` had already shipped.

Lesson: trust the ecosystem's mature single-header / single-source
libraries; ship the differentiator (binding protocol + view registry +
SDK), not the plumbing.

In a hypothetical v2 — when WebSocket support becomes desirable —
we'll re-evaluate the HTTP library. **uWebSockets** is the leading
candidate (Apache-2.0, native WebSocket, business-grade). The
transition would be ABI-compatible from the consumer's perspective:
only `http_adapter.cpp` changes.

### Performance budget

| Metric | Target |
|--------|--------|
| Throughput (state pushes) | 10k frames/s on modern laptop, single SSE client |
| Latency (set_text → SSE delivered) | < 5 ms median, localhost |
| Concurrent SSE clients | 64 (configurable) per `HttpAdapter` |
| Memory per client | < 16 KB |
| Server thread count | `worker_threads` from config |

Anything that needs to be fundamentally faster should use a different
adapter (or a different framework — be honest about the limit).

## Testing strategy

* **Unit tests** (`tests/test_http_adapter.cpp` — doctest):
  registry round-trip, shadow state set/get, subscription lifecycle,
  custom command registration, start/stop on ephemeral port.
* **Integration test** (deferred): spawn a minimal test host, use libcurl
  to hit each REST endpoint and `EventSource`-equivalent to read the
  SSE stream.
* **Manual**: exercise the adapter from a focused local test host; the flagship application's Web support in [AriaTools](https://github.com/dqsjqian/AriaTools) is still a work in progress.

## Future work (deferred, not blocking acceptance)

1. **WebSocket transport.** Add `transport: ws|sse|auto` to
   `HttpAdapterConfig`. v2 protocol bump.
2. **List push.** Implement the `list_changed` envelope to mirror
   `ObservableList<T>` diffs. Currently SSE only pushes scalar state.
3. **Authentication hook.** A `HttpAdapterConfig::auth_callback` slot
   so consumers can plug in cookies/JWT/mTLS without editing the
   adapter.
4. **Replay buffer.** Optional ring buffer of recent state frames so
   re-connecting clients can catch up without a full re-snapshot.
5. **Static-file etag/cache.** Currently re-reads on every request.
6. **`docs/security.md`** with deployment-hardening recipes.
7. **Rust/Python clients** mirroring `aria_client.js`.

## Decision log

* **JSON via `nlohmann::json`, HTTP via `cpp-httplib`** — both vendored
  single-header, MIT-licensed, widely deployed. Reinventing either
  would have cost weeks of work, shipped a worse product, and signaled
  poor engineering judgement. "Minimal dependencies" is a means
  (small build matrix, fast clean checkout) not an end (refusing
  battle-tested libraries to look pure).
* **JSON, not protobuf** — JSON is universal in the browser and our
  payloads are tiny. Protobuf would force a build step on the
  frontend; not worth it for v1.
* **SSE, not WebSocket, in v1** — cpp-httplib doesn't speak WS
  natively. SSE buys ~95% of the ergonomic benefit at ~5% of the
  implementation cost (we get it for free with chunked content
  providers). The protocol envelopes are designed to extend to WS
  in v2 without a breaking change.
* **`HttpView` is logical, not native-handle** — there is no
  "native widget" to wrap; the view is a routing-key + kind pair.
  Keeping the abstraction parallel to QtView / NSViewAdapter / etc.
  preserves the IViewAdapter contract.
* **localhost-by-default** — security is opt-in. Bind to 0.0.0.0
  only after you've added auth + TLS upstream.
* **Reverted: don't hand-write JSON or HTTP** — initial drafts tried
  to ship without external deps. Replaced by nlohmann::json and
  cpp-httplib respectively. Lesson: trust the ecosystem's mature
  single-header libraries; ship the differentiator (the binding
  protocol + view registry + SDK), not the plumbing.
