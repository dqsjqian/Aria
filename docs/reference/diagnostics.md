# Aria Diagnostics Model

> This document is the framework's authoritative reference for the
> **unified diagnostics protocol**.
> Every observable subsystem MUST emit `aria::TraceEvent` per the
> contract below. Together with [`lifecycle.md`](./lifecycle.md),
> [`api-style.md`](./api-style.md) and [`error-model.md`](./error-model.md),
> this file forms the framework's four-pillar contract document family.
> Every contract item is numbered `D-N` for citation in code and commit
> messages.

What a "best-in-class C++ MVVM framework" requires of its diagnostic
protocol:

1. **Unified**: all subsystems publish through one `TraceEvent` type;
   tools consume one shape.
2. **Zero overhead**: when no sink is installed, the cost is one
   `shared_ptr` load + null check — negligible on hot paths.
3. **Non-blocking**: exceptions thrown by a sink never propagate up
   the call stack; the diagnostic path must never pollute the
   business path.
4. **Optional**: sinks can be installed and uninstalled concurrently
   from any thread.

---

## 1. TraceCategory

### D-1: six-value enum, never reordered, append-only

| Category | When it fires | Primary publishers |
|---|---|---|
| `Reactive`   | Graph flush / Pull / Recomputed / SkipClean / Round boundaries | `Graph::flush` |
| `Async`      | AsyncCommand / AsyncResource lifecycle | `classify_async_exception` / Invocation ctor/dtor / AsyncResource fetch |
| `Binding`    | BindingEngine VM↔View dispatch | `BindingEngine::dispatch_to_view_` / `view.on_destroy` callback |
| `Command`    | Synchronous `Command<Args...>` and `Command<>` execution | `Command::execute` / `notify_can_execute_changed` |
| `Validation` | Validator rule evaluation and pending transitions | `Validator::run_` / `begin_pending` / `finish_pending_` |
| `List`       | ObservableList structural mutations | `ObservableList::emit_` |

Numeric ordering is stable — **never reordered**, append-only.

---

## 2. `aria::TraceEvent` value type

```cpp
struct TraceEvent {
    TraceCategory                          category;
    TracePayload                           payload;        // std::variant
    std::chrono::steady_clock::time_point  time;
    std::optional<aria::Error>             error;          // populated only by *_fail / *_error events
};
```

### D-10: payload is a `std::variant`

`TracePayload = variant<Reactive, Async, Binding, Command, Validation, List>`,
aligned with the `TraceCategory` order. `std::get_if<X>(&ev.payload)`
is the standard entry point for consumers wanting strongly-typed field
access.

### D-11: payload struct fields are append-only

Adding a field to `trace::Reactive` / `trace::Async` / etc. is
**forward compatible** — old consumers ignore unknown fields.
Removing a field is **breaking** and MUST be called out in the
CHANGELOG.

### D-12: `error` field on failure events

When the event represents "something failed", the `error` field MUST
be a meaningful `aria::Error`:
- `Async{op="cancelled"}` → `error = Error::cancellation(...)`
- `Async{op="timeout"}` → `error = Error::timeout(...)`
- `Async{op="failure"}` → `error = Error::from_exception(...)`

Successful events leave `error` as `nullopt`; consumers use that to
distinguish.

---

## 3. Sink protocol

### D-20: a sink is `std::function<void(const TraceEvent&)>`

`TraceSink` has no rich API: a sink can be invoked, never queried.
This keeps the diagnostic protocol from leaking into the business
protocol.

### D-21: concurrent safety of registration + publish

Sink registration (`install_trace_sink` / `clear_trace_sink` /
`ScopedTraceSink`) serialises through a global mutex. Publishing
(`publish_trace`) takes a single `lock_guard`-scoped copy of the
sink's `shared_ptr`, then invokes the sink **outside the lock**.

Implications:

- During a publish, other threads may install / replace / clear the
  sink concurrently; the new sink only takes effect from the **next**
  publish onward — the in-flight publish keeps its strong reference
  to the previous sink, which therefore cannot be freed mid-call.
- If the sink itself needs concurrent safety internally, it must
  arrange for that on its own.

### D-22: sink exceptions **never** propagate

`publish_trace` / `publish_trace_unchecked` wrap the call in
`try { sink(ev); } catch (...) { /* swallow */ }`. Sink throws are
swallowed; the business path continues unaffected.

### D-23: `ScopedTraceSink` is the test-side primitive

Tests SHOULD use `ScopedTraceSink`: it installs on construction and
**restores** the previous state on destruction (which may be no sink
or an outer scoped sink). This lets tests nest in parallel without
bleeding into one another.

### D-24: zero-overhead contract

Exact costs (per the call-site gating convention):

| Path | Real cost |
|---|---|
| **Fast path** (no sink) | One `shared_ptr` snapshot + null check at the call site (`if (has_trace_sink()) { ... }`); the inner block never runs. |
| **Slow path** (sink present) | One snapshot at the call site (gating) + one snapshot inside `publish_trace_unchecked` (to invoke) = **2 snapshots**; payload construction happens only on the slow path. |

The two publish entry points are deliberately split (D-1 implementation
detail):

- **`publish_trace_unchecked(...)`** trusts the caller to have done the
  `has_trace_sink()` gate; it does no internal redundant check. Every
  subsystem hooks via this path, matching the slow-path budget above.
- **`publish_trace(...)`** is still provided and short-circuits on
  `has_trace_sink()` itself, for cold paths that don't bother gating.

Every subsystem guards publishes with
`if (has_trace_sink()) { ... publish_trace_unchecked(...); }` —
mandatory whenever payload construction is non-trivial.

---

## 4. Per-subsystem hook points

### D-30: Reactive

`Graph::flush` emits one `trace::Reactive` per phase boundary:

| phase | When | node_name | round | changed |
|---|---|---|---|---|
| `FlushBegin`  | start of flush | empty | 0 | false |
| `RoundBegin`  | start of each round | empty | 1..N | false |
| `Pull`        | before pulling each dirty node | node debug name | current round | false |
| `SkipClean`   | already-Clean node skipped | node debug name | current round | false |
| `Recomputed`  | after recompute | node debug name | current round | whether the value actually changed |
| `RoundEnd`    | end of each round | empty | current round | false |
| `FlushEnd`    | end of flush | empty | total rounds | false |

Note: the reactive subsystem **also** keeps the legacy
`GraphInspector::install_flush_tracer` protocol (FlushTracer +
FlushEvent). The two coexist — the former is for "I only care about
reactive internals" fine-grained debugging, the latter is the unified
diagnostic. Both are independently controllable.

### D-31: Async

`AsyncCommand::Invocation` fires `execute_start` / `execute_finish`
in its ctor / dtor, with `generation` set to the inflight count at
that moment. `classify_async_exception` produces one event per
branch:

| op | Trigger | error field |
|---|---|---|
| `cancelled` | `OperationCancelled` | `Error::cancellation(...)` |
| `timeout`   | `TimeoutError` | `Error::timeout(...)` |
| `failure`   | other | `Error::from_exception(...)` |

`AsyncResource` exposes finer-grained events: `cache_hit` / `dedupe`
/ `fetch_start` / `fetch_finish` / `stale_drop` / `cancelled` /
`timeout` / `failure`. `generation` is that fetch's `gen` counter.

### D-32: Binding

`BindingEngine::dispatch_to_view_` produces one of:

| op | When |
|---|---|
| `vm_to_view`            | The user callback actually ran. |
| `view_destroyed_drop`   | The alive_token expired, posted callback was dropped. |

`view.on_destroy` fires one `view_destroyed`.

### D-33: Command

`Command::execute` and `Command<>::execute`:

| op | When |
|---|---|
| `execute`              | Predicate passed; action is about to run. |
| `rejected_can_execute` | Predicate rejected. |
| `can_execute_changed`  | `notify_can_execute_changed` was called. |

### D-34: Validation

`Validator::run_` emits `rule_pass` or `rule_fail` per rule (warnings
do `warning_pass` / `warning_fail`). `key` is `(field_path, rule_id)`;
`*_fail` also includes `message`. `begin_pending` / `finish_pending_`
each fire once.

### D-35: List

`ObservableList::emit_` mirrors every structural change broadcast to
a `trace::List`:

| op | Meaning | index | from_index | size_after |
|---|---|---|---|---|
| `Insert`      | New element added | insertion index | 0 | size after insert |
| `Remove`      | Element removed | removal index | 0 | size after remove |
| `Replace`     | Element replaced | index | 0 | unchanged |
| `ItemChanged` | T's own on_changed | index | 0 | unchanged |
| `Move`        | Element moved | target index | source index | unchanged |
| `Reset`       | Cleared | 0 | 0 | 0 |

### D-36: subsystems intentionally NOT hooked

The following subsystems are deliberately out of the unified sink:

- **Property::set / Computed::recompute**: the pure reactive
  movement is already captured by `Reactive`'s Pull / Recomputed
  events; surfacing it again would be noise.
- **Effect**: same as above.
- **EventBus**: no observable failure surface today, and the broadcast
  itself is already a user-defined event protocol; layering more
  diagnostics on top has limited value. If we ever introduce
  "slow-handler detection", revisit.

---

## 5. Anti-patterns

| # | Anti-pattern | Consequence | Correct approach |
|---|---|---|---|
| AD1 | Sink throws to interrupt the business | Exception is silently swallowed; business proceeds normally | Don't throw from a sink. To interrupt the business, surface state through a real Property (e.g. `last_error`) |
| AD2 | Constructing a heavy payload BEFORE checking `has_trace_sink()` | Pay the cost even when nobody's listening | Always gate with `if (has_trace_sink()) { ... publish_trace_unchecked(...); }` (D-24) |
| AD3 | Sink performs heavy work (file I/O, network) | Slows the hot path (every reactive flush / list mutation triggers it) | Sink should enqueue lightly; offload heavy work to a background thread |
| AD4 | Test calls `install_trace_sink` and forgets to clear | Subsequent tests pick up stale events | Use `ScopedTraceSink` for automatic restoration |
| AD5 | Cross-thread sink invocation that assumes thread-safety | Sink internals race | Assume the sink may be called from any thread; lock internally |

---

## 6. Cross-document references

- [`lifecycle.md`](./lifecycle.md) **L-13** "unsubscribe during emit":
  the diagnostic sink's "snapshot-then-invoke" pattern is isomorphic
  to the ABI signal one.
- [`error-model.md`](./error-model.md) **E-12** Errors must carry a
  stable `source`: the `Async` category's `source` field reuses the
  same stable labels.
- [`api-style.md`](./api-style.md) **S-30** template diagnostic
  priorities: `publish_trace` uses `requires` to constrain the
  payload type — compiler emits one-line diagnostics on misuse.

---

## 7. P0-ε target mapping (TODO)

| Invariant | fuzzer |
|---|---|
| Install/clear sink does not race | `trace_sink_install_race_fuzzer` |
| 1M `publish_trace` calls with no sink installed have negligible latency | `trace_sink_zero_overhead_bench` |
| Sink throws don't propagate | `trace_sink_throw_swallow_fuzzer` |
| Nested `ScopedTraceSink` installs/restores correctly | `trace_sink_scoped_nesting_fuzzer` |

---

## 8. Document governance

Every diagnostic-protocol change MUST flow as: doc change → code
change → test change. Any new `TraceCategory` MUST first be
registered in the D-1 table here; new payload fields are recorded
under D-11.
