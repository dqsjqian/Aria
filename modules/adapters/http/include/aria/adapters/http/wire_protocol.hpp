#pragma once
/// @file wire_protocol.hpp
/// @brief Wire protocol v1 for the HTTP adapter.
///
/// All payloads are JSON, UTF-8 encoded. The protocol is intentionally
/// simple — anything that needs to be efficient should use a different
/// adapter. The HTTP adapter is for "ergonomic and ubiquitous", not for
/// "fastest".
///
/// # Topology
///
///   Browser/JS ──HTTP/REST──▶ HttpAdapter ──ViewModel──▶ Aria core
///        ▲                          │
///        └────────SSE stream────────┘
///
/// SSE (Server-Sent Events) is used for server→client push. WebSockets
/// would be marginally better but are not natively supported by the
/// embedded HTTP server (cpp-httplib v0.18). v2 of the protocol is
/// planned to add WebSocket as the preferred transport with SSE as
/// fallback.
///
/// # Endpoints
///
/// | Method | Path                | Purpose                                |
/// |--------|---------------------|----------------------------------------|
/// | GET    | /aria/health        | Readiness probe                        |
/// | GET    | /aria/views         | Enumerate registered views             |
/// | GET    | /aria/state?view=X  | Snapshot of one view's shadow state    |
/// | POST   | /aria/state         | Update a view's value (JSON body)      |
/// | POST   | /aria/click         | Fire a click event                     |
/// | POST   | /aria/command       | Invoke a custom command                |
/// | GET    | /aria/stream        | Subscribe to server-sent events        |
///
/// # Message envelopes
///
/// **Server → Client (SSE)** — `data:` lines carry JSON of these shapes:
///
///   {"type":"hello","platform":"http","protocol":1}
///   {"type":"state","view":"<id>","field":"text|bool|int|double","value":<v>}
///   {"type":"event","view":"<id>","field":"click"}
///   {"type":"list","view":"<id>","op":"insert|remove|replace|reset|move",
///                  "index":<n>,"to":<n>,"value":<json>}
///   {"type":"visibility","view":"<id>","value":true|false}
///   {"type":"enabled","view":"<id>","value":true|false}
///   {"type":"error","message":"<text>"}
///   {"type":"ping"}                       // every 25s for keep-alive
///
/// **Client → Server (REST body)**:
///
///   POST /aria/state    {"view":"<id>","field":"text","value":"hello"}
///   POST /aria/click    {"view":"<id>"}
///   POST /aria/command  {"view":"<id>","command":"<name>","args":<json>}
///
/// Servers MUST tolerate unknown fields (forward-compat). Clients MUST
/// tolerate unknown `type` values (forward-compat).

#include "aria/abi/export.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace aria::adapters::http::wire {

/// Protocol version. Incremented on incompatible changes only.
inline constexpr int kProtocolVersion = 1;

/// Default API path prefix. All endpoints are mounted under this.
inline constexpr const char* kDefaultApiPrefix = "/aria";

/// Default heartbeat interval (seconds). Servers emit a `ping` event
/// at this cadence to keep SSE connections alive across proxies.
inline constexpr int kDefaultHeartbeatSec = 25;

/// Field-name string constants used in JSON envelopes. Centralised here
/// so client SDKs (web-sdk/aria_client.js) and server emitters cannot
/// diverge by accidental typo.
namespace fields {
inline constexpr const char* kType    = "type";
inline constexpr const char* kView    = "view";
inline constexpr const char* kField   = "field";
inline constexpr const char* kValue   = "value";
inline constexpr const char* kCommand = "command";
inline constexpr const char* kArgs    = "args";
inline constexpr const char* kOp      = "op";
inline constexpr const char* kIndex   = "index";
inline constexpr const char* kTo      = "to";
inline constexpr const char* kMessage = "message";
}  // namespace fields

namespace event_types {
inline constexpr const char* kHello      = "hello";
inline constexpr const char* kState      = "state";
inline constexpr const char* kEvent      = "event";
inline constexpr const char* kList       = "list";
inline constexpr const char* kVisibility = "visibility";
inline constexpr const char* kEnabled    = "enabled";
inline constexpr const char* kError      = "error";
inline constexpr const char* kPing       = "ping";
}  // namespace event_types

namespace field_kinds {
inline constexpr const char* kText   = "text";
inline constexpr const char* kBool   = "bool";
inline constexpr const char* kInt    = "int";
inline constexpr const char* kInt64  = "int64";
inline constexpr const char* kUInt64 = "uint64";
inline constexpr const char* kFloat  = "float";
inline constexpr const char* kDouble = "double";
inline constexpr const char* kClick  = "click";
}  // namespace field_kinds

namespace list_ops {
inline constexpr const char* kInsert  = "insert";
inline constexpr const char* kRemove  = "remove";
inline constexpr const char* kReplace = "replace";
inline constexpr const char* kReset   = "reset";
inline constexpr const char* kMove    = "move";
}  // namespace list_ops

}  // namespace aria::adapters::http::wire
