# HTTP / REST / SSE Adapter

Integrating Aria with web frontends via HTTP + Server-Sent Events. The HTTP adapter is the "web sibling" of Qt/AppKit/UIKit adapters — same `IViewAdapter` contract, same `BindingEngine` wiring, but the "views" are remote browser elements communicating over REST + SSE.

**Include:** `#include "aria/adapters/http/http_adapter.hpp"`

---

## Architecture

```
┌──────────────────────────────────────────────────┐
│  Browser                                         │
│  ┌──────────┐  SSE stream  ┌─────────────────┐ │
│  │ JS SDK   │ ◄────────── │ HttpAdapter      │ │
│  │ (vanilla)│ ──────────► │ (C++ server)     │ │
│  └──────────┘  REST POST   └────────┬────────┘ │
└───────────────────────────────────────┼──────────┘
                                        │ IViewAdapter
                              ┌─────────▼──────────┐
                              │  BindingEngine      │
                              │  + ViewModel        │
                              └─────────────────────┘
```

- **VM→View**: Property changes are pushed to all connected browsers via SSE
- **View→VM**: Browser interactions arrive as REST POST requests
- **No polling**: SSE provides real-time push; REST provides immediate user-input delivery

---

## Setup

### Create and Configure

```cpp
#include "aria/adapters/http/http_adapter.hpp"
#include "aria/adapters/http/http_config.hpp"

aria::adapters::http::HttpAdapterConfig config;
config.host = "0.0.0.0";
config.port = 9090;           // 0 = OS picks a free port
config.rate_limit_per_sec = 30;  // SSE throttle

aria::adapters::http::HttpAdapter http(config);
```

### Register Views

Views are identified by a stable string ID and a kind:

```cpp
// kind ∈ {"text","bool","int","int64","uint64","float","double","click"}
auto& search_field = http.register_view("search_query", "text");
auto& submit_btn   = http.register_view("submit", "click");
auto& result_label = http.register_view("result", "text");
auto& count_spin   = http.register_view("item_count", "int");
```

### Bind with BindingEngine

```cpp
aria::binding::BindingEngine engine(http);

engine.bind_text(vm.query, search_field);
engine.bind_command(vm.submit_cmd, submit_btn);
engine.bind_text_oneway(vm.result, result_label);
engine.bind_int(vm.count, count_spin);
```

### Start the Server

```cpp
http.start();   // Non-blocking; spawns server thread

// Find out which port was bound (useful when config.port == 0)
std::cout << "Listening on port " << http.actual_port() << "\n";
```

### Stop

```cpp
http.stop();    // Closes all SSE connections; idempotent
```

---

## Wire Protocol

### SSE Stream (VM → Browser)

Clients connect to `GET /sse` and receive a stream of JSON events:

```json
{"view":"search_query","kind":"text","value":"hello"}
{"view":"item_count","kind":"int","value":42}
```

### REST Endpoints (Browser → VM)

| Endpoint | Purpose |
|----------|---------|
| `GET /` | Serve the built-in dashboard HTML |
| `GET /sse` | Open SSE connection |
| `GET /views` | List registered views |
| `POST /aria/set` | Write a value to a view (`{view, kind, value}`) |
| `POST /aria/command` | Invoke a command (`{view, command, args}`) |

### Browser-Side: Vanilla JS SDK

```html
<script src="/sdk.js"></script>
<script>
const aria = new AriaSDK();  // auto-connects to /sse

// Read current state
console.log(aria.views);

// Write a value
aria.set("search_query", "hello world");

// Listen for changes
aria.on("search_query", (value) => {
    document.getElementById("result").textContent = value;
});
</script>
```

No build step, no npm. Works with React/Vue/Svelte by wrapping the SDK in a component hook.

---

## Custom Commands

Register server-side handlers for custom browser commands:

```cpp
http.register_command("submit", "validate",
    [](std::string_view args_json) -> std::string {
        // Parse args_json, run validation, return JSON result
        return R"({"valid":true})";
    });
```

Browser sends:
```json
POST /aria/command
{"view":"submit","command":"validate","args":{"field":"email"}}
```

---

## View Registry

```cpp
// Find a previously registered view
auto* v = http.find_view("search_query");

// List all views
for (const auto& info : http.list_views()) {
    std::cout << info.id << " (" << info.kind << ")\n";
}

// Unregister (fires on_destroy so BindingEngine cleans up)
http.unregister_view("search_query");
```

---

## Native Server Escape Hatch

Access the underlying `cpp-httplib::Server` for custom routes:

```cpp
// Must be called BEFORE start()
auto& srv = http.native_server();
srv.Get("/api/status", [](const auto& req, auto& res) {
    res.set_content("{\"ok\":true}", "application/json");
});
```

> **Warning:** This is an unstable API — it leaks the implementation detail that the adapter uses `cpp-httplib`. If the HTTP backend changes, this signature changes. Prefer `register_command()` for stable extensibility.

---

## Threading

- `register_view` / `unregister_view` / `start` / `stop` are thread-safe
- IViewAdapter setters (set_text, set_bool, ...) may be called from any thread — they enqueue an SSE broadcast and return without blocking
- IViewAdapter getters return the current shadow state under a shared mutex
- User callbacks (on_text_changed, on_click, ...) fire on HTTP server worker threads — marshal back to the UI thread if needed

---

## Configuration Reference

| Option | Default | Description |
|--------|---------|-------------|
| `host` | `"127.0.0.1"` | Listen address |
| `port` | `9090` | Listen port (0 = OS picks) |
| `rate_limit_per_sec` | `0` (off) | Max SSE events/sec per client |
| `cors_origin` | `""` (none) | CORS Allow-Origin header |

---

## Quick Reference

| Method | Description |
|--------|-------------|
| `register_view(id, kind)` | Register a logical view |
| `find_view(id)` | Lookup by ID |
| `unregister_view(id)` | Remove and fire on_destroy |
| `list_views()` | Snapshot of (id, kind) pairs |
| `register_command(view_id, name, handler)` | Custom command handler |
| `start()` | Start server thread |
| `stop()` | Stop server, close SSE |
| `running()` | Is server alive |
| `actual_port()` | Bound port (0 if stopped) |
| `client_count()` | Connected SSE clients |
| `native_server()` | Escape hatch to cpp-httplib |

---

## See Also

- [View Binding →](../binding.md) — BindingEngine API reference
- [RFC 0001 — HTTP Adapter →](../../rfc/0001-http-adapter.md)
- [AriaTools →](https://github.com/dqsjqian/AriaTools) — flagship cross-platform application; Web support is a work in progress
