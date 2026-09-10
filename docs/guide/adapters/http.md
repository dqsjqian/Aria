# HTTP / REST / SSE Adapter

The HTTP adapter exposes registered logical views to browsers using REST and
Server-Sent Events. It implements the same `IViewAdapter` contract as native UI
adapters. Build with `-DARIA_BUILD_HTTP=ON`; add `-DARIA_HTTP_ENABLE_TLS=OFF`
for a plain HTTP development build. Link the application to `aria::http` and
`aria::runtime` when using `SimpleDispatcher`.

## A compiled starting point

This complete example is compiled and run as `http_guide_example` in CTest.
It starts and stops immediately; in an application, keep the server alive and
pump the dispatcher from the thread that owns the reactive graph, or supply
the host's existing `IDispatcher`. Binding teardown also belongs on that thread.

<!-- BEGIN COMPILED HTTP EXAMPLE -->
```cpp
#include "aria/adapters/http/http_adapter.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/runtime/dispatcher.hpp"
#include <memory>
#include <string>

int main() {
    aria::adapters::http::HttpAdapterConfig config;
    config.port = 0;
    config.worker_threads = 4;
    auto http = std::make_shared<aria::adapters::http::HttpAdapter>(config);
    auto dispatcher = std::make_shared<aria::runtime::SimpleDispatcher>();
    aria::Property<std::string> query("hello");
    aria::binding::BindingEngine engine(http, dispatcher,
        aria::binding::BindingEngine::DispatchPolicy::SmartMarshal);
    auto& search = http->register_view("search_query", "text");
    engine.bind_text(query, search);
    if (!http->start()) return 1;
    // A real host repeatedly pumps this dispatcher on the graph owner thread.
    dispatcher->pump();
    http->stop();
    return http->actual_port() == 0 ? 0 : 1;
}
```
<!-- END COMPILED HTTP EXAMPLE -->

`start()` waits for listening readiness and returns false on failure or if
already running. `actual_port()` reports the selected port and resets to zero
on stop. `stop()` closes streams and interrupts the heartbeat wait. Call
start/stop from the host lifecycle thread, outside HTTP callbacks.

Supported view kinds: `text`, `bool`, `int`, `int64`, `uint64`, `float`,
`double`, `click`. Use `bind_command(command, click_view)` for commands.
Registering an existing ID replaces its view, drops old subscriptions and
custom commands, and clears its shadow state. Existing references to that
view become invalid; bind the replacement returned by `register_view`.

## Endpoints and protocol 2

All endpoints use `config.api_prefix`, default `/aria`.

| Method and path | Purpose |
|---|---|
| `GET /aria/health` | Readiness and protocol version |
| `GET /aria/views` | `{ "views": [{"id":"…", "kind":"…"}] }` |
| `GET /aria/state?view=X` | `{view, kind, value, visible, enabled}` |
| `POST /aria/state` | `{view, field, value}` |
| `POST /aria/click` | `{view}` for a click view |
| `POST /aria/command` | `{view, command, args}` for a custom handler |
| `GET /aria/stream` | SSE stream |

The server serves files at `/` only when `static_root` points to a directory.
There is no built-in dashboard or automatic SDK mount.

SSE frames contain JSON on `data:` lines. Initial delivery is `hello`, then
registered views' current values (except click views), visibility and enabled
state. Unset values are null; visibility and enabled default to true. Initial
snapshots precede subsequent state changes for that connection.

```json
{"type":"hello","platform":"http","protocol":2}
{"type":"state","view":"search_query","field":"text","value":"hello"}
{"type":"visibility","view":"search_query","value":true}
{"type":"enabled","view":"search_query","value":true}
```

`int64` and `uint64` values outside JavaScript's safe integer range
[-9007199254740991, 9007199254740991] travel as exact decimal strings with the
same field tag. Safe values remain JSON numbers. The SDK returns unsafe values
as `BigInt`. For display use `String(value)`; for JSON serialization explicitly
convert BigInt to a decimal string. A protocol 1 client that assumes every
numeric field is a Number must be upgraded together with the server.

The server accepts range-checked integer JSON tokens and, for 64-bit fields,
decimal strings. The SDK accepts safe Number, bigint or decimal string for
`setInt64` / `setUInt64`; it rejects already-rounded unsafe Numbers. Fractional
integers, bool-as-number, negative unsigned values and out-of-range numbers
receive 400. Bad JSON/schema or a field that mismatches its view also receive
400; unknown views/commands receive 404. Errors carry `{ "error": "…" }`.

## Browser SDK

Copy `modules/adapters/http/web-sdk/aria_client.js` into your served static
root and import it as an ES module. No npm dependencies are needed.

```html
<script type="module">
import { AriaClient } from './aria_client.js';
const client = new AriaClient(location.origin);
client.onOpen(() => console.log('connected'));
client.onError(error => console.error(error));
client.subscribe('search_query', value => {
    document.getElementById('result').textContent = value ?? '';
});
await client.connect();
await client.setText('search_query', 'hello world');
// On component/application teardown: client.close();
</script>
```

Connection errors clear `isConnected()`; EventSource can reconnect automatically,
and each new hello triggers `onOpen`. Closing rejects a pending connection.
Fetch helpers reject non-success HTTP statuses. State subscriptions can also
use the `viewId.visibility` and `viewId.enabled` keys.

## Threading and extensibility

HTTP subscriptions and custom command handlers run on server worker threads.
Use `BindingEngine` with a dispatcher and `SmartMarshal` or `AlwaysPost` to
route bound Property and Command work onto the graph owner thread. A successful
POST acknowledges validation and dispatch; queued model work may still be
pending. Direct adapter subscriptions and custom handlers must explicitly
marshal any graph access themselves.

Registry maps and shadow state are mutex protected; getters return copies.
Setters enqueue SSE data without waiting for socket writes. The returned view
references are not lifetime pins: serialize registration/replacement/removal
with binding and native view use on the graph owner thread. Avoid removing a
view while another thread dereferences it.

`register_command(view_id, name, handler)` receives a JSON argument string and
returns a JSON response string. The view must exist when invoked.
`native_server()` allows custom cpp-httplib routes before start, but is an
unstable escape hatch and requires the dependency's headers. Replacing its
worker queue or blocking custom handlers changes the adapter's capacity
assumptions.

## Configuration

| Field | Default | Meaning |
|---|---|---|
| `host` | `127.0.0.1` | Bind address |
| `port` | `9090` | 0 selects an available port |
| `api_prefix` | `/aria` | REST/SSE path prefix |
| `static_root` | empty | Optional static-file directory |
| `worker_threads` | `0` | Fixed pool; 0 detects CPU count, minimum 2; explicit counts must be ≥2 |
| `max_sse_clients` | `64` | Also capped at workers minus one; 0 removes only this extra cap |
| `heartbeat_sec` | `25` | Positive interval in seconds |
| `enable_cors` | `false` | Adds permissive CORS headers when enabled |
| `tls_cert_file`, `tls_key_file` | empty | PEM pair for a TLS-enabled build |
| `tls_ca_file` | empty | Optional client-certificate verification CA |
| `tls_min_version` | `1.2` | `1.2` or `1.3` |

Excess SSE connections receive 503 so a worker remains available for REST.
This is a connection limit, not event rate limiting. Long-running custom REST
handlers can still consume the remaining workers. The default address is local;
external deployments must provide their own authentication/network boundary.

See [binding](../binding.md), the [protocol header](../../../modules/adapters/http/include/aria/adapters/http/wire_protocol.hpp)
and [historical RFC](../../rfc/0001-http-adapter.md).
