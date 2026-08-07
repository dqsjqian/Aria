# Aria Error Model

> This document is the framework's authoritative reference for the
> **error protocol**.
> Every observable error surface MUST emit `aria::Error` values per
> the contract below. Together with [`lifecycle.md`](./lifecycle.md)
> and [`api-style.md`](./api-style.md), this file forms the framework's
> three-pillar contract document family. Every contract item is
> numbered `E-N` for citation in code and commit messages.

What a "best-in-class C++ MVVM framework" requires of its error model:

1. **Unified**: every observable error surface uses the same value
   type (`aria::Error`); a user does not need to learn fresh
   semantics per surface.
2. **Locatable**: every `Error` carries `kind` + `source` + an
   optional `key`. UI / logs / routing can dispatch without parsing
   message strings.
3. **Never silent**: errors must be observable — either as a
   `Property<std::optional<Error>>` you can subscribe to, or as a
   thrown exception that the call site sees immediately.
4. **Cancellation is not an error**: `OperationCancelled` MUST NOT
   appear on `last_error`-style observation surfaces; it is
   control flow, not an observation event.

---

## 1. ErrorKind taxonomy

### E-1: eight-value enum, never reordered, append-only

| Kind | When it fires | Typical source |
|---|---|---|
| `UserError` | Caller passed something that obviously violates the API contract (null view-model, out-of-range, wrong type) | "Navigator", "Container" |
| `Validation` | Validator / FormValidator rule failed | "Validator", "FormValidator" |
| `AsyncFailure` | Async body threw a non-cancel, non-timeout exception | "AsyncCommand", "AsyncResource" |
| `Cancellation` | Coroutine exited via `OperationCancelled` (**diagnostic surfaces only** — `last_error` never carries this kind) | "AsyncCommand" |
| `Timeout` | `with_timeout` deadline hit (Race / Fail modes both map here) | "AsyncCommand", "with_timeout" |
| `BindingFailure` | Reserved for a future explicit binding error surface; current binding failures are reported through `aria::CallbackFailure` / `callback_failure_sink` (see E-30) | "BindingEngine", "<adapter>" |
| `GraphCycle` | Reactive graph reached `kMaxFlushRounds`, `CircularDependencyError` promotion | "Graph" |
| `InvariantViolation` | The framework's own contract was breached (stress / fuzz reports only) | "Validator", "Graph", ... |

### E-2: enum order is stable

`ErrorKind` numeric ordering is stable, **never reordered**, append-only.
Reason: the enum crosses the ABI boundary (used as the value of
`Property<std::optional<Error>>`); reordering would break dylib
compatibility.

### E-3: Severity is meaningful only for Validation

`Severity::Warning` is used **only** when `kind == Validation`: it
denotes a soft advisory (`should()` rule). For every other kind the
severity MUST be `Severity::Error`; framework code does not
construct `Warning`s outside Validation.

UI rendering convention:
- Any `Severity::Warning` → yellow banner.
- Any `Severity::Error` with `kind != Cancellation` → red error.
- `kind == Cancellation` → don't render (control flow, not an error).

---

## 2. `aria::Error` value type

```cpp
struct Error {
    ErrorKind                 kind;       // primary routing field
    Severity                  severity;
    std::string               message;    // human-readable
    std::string               source;     // subsystem tag
    ValidationKey             key;        // meaningful only for Validation
    std::exception_ptr        inner;      // optional escape hatch
};
```

### E-10: `Error` satisfies `PropertyValue`

`Error` is copyable + EqualityComparable, so it can be the `T` of a
`Property<std::optional<Error>>`.

### E-11: `inner` does **not** participate in equality

`exception_ptr` is pointer identity, useless for value equality. The
`Property` write-equality gate (L-21) compares `kind / severity /
message / source / key`. Consequence: writing the same logical error
twice does NOT re-notify observers.

### E-12: every Error must have a `source`

Factory functions (`Error::async_failure / cancellation / timeout /
user_error / graph_cycle / validation / validation_warning`) all take
a `source_tag` parameter (with sensible defaults). Constructing an
`Error` with an empty source and feeding it to an observation surface
is **forbidden**; empty source is reserved for unit-test internals.

### E-13: `from_exception` is a degraded mapping

`Error::from_exception(exception_ptr, source)` recognises only
**standard library** exceptions:

| Exception | Maps to |
|---|---|
| `std::invalid_argument` | `UserError` |
| `std::out_of_range`     | `UserError` |
| Other `std::exception`  | `AsyncFailure` (`inner` retained) |
| Unknown                 | `AsyncFailure("unknown error")` |

**Aria's own sentinel exceptions** are NOT recognised inside
`from_exception` — that would force `error.hpp` to back-include
`async/`, `reactive/` and break the layering. Each error surface
(`classify_async_exception` / a future `Graph::handle_cycle` / ...)
catches its sentinels first, calls a precise factory, and only lets
the residue flow into `from_exception`.

---

## 3. Per-surface protocol

### E-20: AsyncCommand — rich + string twin observation surfaces

`AsyncCommand` exposes:

- `last_error: Property<std::optional<aria::Error>>` — primary, rich
  payload.
- `last_error_message: Property<std::string>` — string projection,
  UI-friendly.
- `last_result: Property<std::optional<R>>` — only when `R != void`.

**Contract**:

1. Every `execute()` start (the 0→1 inflight edge) clears both
   (`nullopt` / `""`).
2. Async body threw `OperationCancelled` → both untouched (NOT
   recorded as an error).
3. Async body threw `TimeoutError` → writes
   `Error::timeout("AsyncCommand")`; message is the original
   `e.what()`.
4. Async body threw anything else → writes
   `Error::from_exception(ex, "AsyncCommand")`.
5. `last_error_message` and `last_error->message` stay in sync; UI
   that only needs the string binds `last_error_message`; UI that
   needs `kind` or routing-actions binds `last_error`.
6. The Property write-equality gate guarantees: writing the same
   error twice does NOT re-notify.

### E-21: AsyncResource — same protocol as AsyncCommand

`AsyncResource<T>` exposes:

- `error: Property<std::optional<aria::Error>>`
- `error_message: Property<std::string>`
- `data: Property<std::optional<T>>`
- `is_loading: Property<bool>`

**Contract**:

1. Every `do_fetch_` start clears both error properties.
2. SWR (stale-while-revalidate): on failure `data` is **NOT**
   cleared — the previous successful result stays so the UI does not
   blink to empty.
3. `kind` mapping is identical to E-20.
4. `Cancellation` does NOT surface on the error face.

### E-22: Validator / FormValidator — the Error list inside ValidationState

`ValidationState.errors: vector<Error>` and `.warnings: vector<Error>`,
each with `kind = Validation` and a populated `key`.

**Contract**:

1. Every Error's `key.field_path = the field_path supplied to the
   Validator's ctor`; `key.rule_id` is either explicitly provided via
   `must(..., ..., rule_id)` or auto-generated as `"rule_<N>"`.
2. `source` is always `"Validator"` or `"FormValidator"` (cross-field
   rule).
3. `severity = Error` lands in `errors`; `severity = Warning` lands
   in `warnings`.
4. `ValidationState.first_error()` returns `optional<Error>` (NOT a
   string).
5. `FormValidator.first_error: Property<string>` and
   `first_error_full: Property<optional<Error>>` — twin projections;
   contract is identical to E-20.
6. When a cross-field rule fails: `key.field_path == ""`,
   `key.rule_id` defaults to `"form_rule_<N>"` or whatever explicit
   `rule_id` was provided. This Error takes **priority** in
   `first_error_full` over any field-level error.

### E-23: Graph cycle — `CircularDependencyError` exception → `Error::graph_cycle()`

The reactive graph's cycle detector still **throws**
(`CircularDependencyError`'s API stays stable, since it fires on
construction- / set-time synchronous paths and the caller needs to
know immediately).

But synchronous-path catchers (e.g. inside `AsyncCommand`'s set
chain) SHOULD wrap the exception into
`Error::graph_cycle(e.what(), current_exception())` and write it to
the surface. `classify_async_exception` does NOT recognise
`CircularDependencyError` today — that is a **current gap**, but
since cycles are synchronous graph errors they should not normally
arise inside an async body. **The P0-ε fuzzer MUST verify this
boundary.**

### E-24: Navigator — synchronous parameter validation still throws `std::invalid_argument`

`Navigator::push(nullptr)` still throws `std::invalid_argument`.

**Rationale**: Navigator is invoked synchronously by user code; a
parameter error must be reported to the caller immediately or it
gets deferred until the first stack-top access. `throw` is the
elegant "never silent" form for this scenario.

If the caller invokes Navigator from inside an `AsyncCommand` body
without catching it, `classify_async_exception` (per E-13) maps it
to `UserError` via `Error::from_exception` — the error is still
observable.

### E-25: Container / DI — same protocol as Navigator

`Container::resolve<I>()` throws `std::runtime_error` when `I` was never
registered. Semantics are identical to E-24: resolution happens
synchronously in user code, so a missing registration is reported to the
caller immediately rather than deferred.

There is no `std::invalid_argument` path — the container accepts no
user-supplied values it could reject, only type keys. (`std::any_cast`
inside `resolve` can in principle raise `std::bad_any_cast` if the same
`type_index` is registered from two DSOs with incompatible types, but that
is a build-configuration fault rather than a documented API outcome.)

### E-26: BindingEngine — **no Property-shaped error surface today**

`BindingEngine` does NOT expose a Property-shaped error observation
surface. Reasons:

1. View-adapter setters' contract is "idempotently update the native
   widget" — they SHOULD NOT throw. If one does, that is an adapter
   implementation bug.
2. The View → VM path is user input; input validity SHOULD be
   handled by the ViewModel's Validator, not by BindingEngine
   reporting "input parse failed".
3. If an adapter setter does throw, the path is:
   `BindingEngine::dispatch_to_view_`'s lambda runs in a slot dispatched
   by the dispatcher → the dispatcher implementation
   (`SimpleDispatcher::pump` / `MainThreadExecutor::drain` /
   `MainThreadExecutor::run_one`) catches at its try/catch boundary
   and forwards via
   `aria::report_callback_failure(category, std::current_exception())`
   → the host application bridges to `aria::Logger::error(category,
   what)` via `aria::runtime::install_default_diagnostics()`. The
   whole chain is `noexcept`; nothing reaches `std::terminate`.
4. `dispatch_to_view_` itself has no try/catch — its job is "post +
   liveness check + trace"; exceptions sink down to the dispatcher
   boundary where the unified callback_boundary handles them. Its
   trace funnels through two non-template helpers,
   `trace_drop_(platform)` / `trace_emit_(platform)`, in all three
   branches (Direct / SmartMarshal-on-thread / dispatcher.post),
   avoiding duplication and template bloat (see the "BindingEngine
   trace helper" section in [api-style.md](api-style.md)).

**`ErrorKind::BindingFailure` is reserved**: if we ever introduce an
"explicit binding error surface" (e.g. a typed converter failing on
VM→View), it activates then. **No code in the current release
emits `ErrorKind::BindingFailure`.** Synchronous callback / converter /
view-model boundary failures instead route through
`aria::report_callback_failure(...)` and the host-installed
`callback_failure_sink`; adapter setters remain expected to be
idempotent and non-throwing.

### E-27: Logger contract — **never throws, never silently drops a message**

`aria::runtime::Logger::log` is the framework's lowest-level
observability primitive. It must be callable from any call site,
including framework-internal `noexcept` boundaries (worker / drain /
pump / abi trampoline). The contract:

1. **Never throws**: `Logger::log` is not declared `noexcept` in the
   signature (we keep it non-noexcept for compatibility with
   `Sink = std::function<...>`), but the implementation **never
   propagates an exception out**. If a user-installed sink throws:
   - `std::exception` → fall back to stderr:
     `[<LEVEL>][<category>] <message>  (sink threw: <e.what()>)`.
   - Non-std exceptions (`throw 42` etc.) → fall back to stderr:
     `(sink threw: non-std exception)`.
   - Both paths are tied off via try/catch inside `Logger::log`;
     nothing escapes back to the caller (especially the
     framework-internal `noexcept` boundaries).
2. **Never silent**: when the sink throws, the original log record
   **still** goes to the stderr fallback with a "sink threw" tag —
   keeping observability while not losing the message.
3. **Pairs with callback_boundary**: when the host registers
   `aria::Logger` as the callback_boundary sink (the default
   `install_default_diagnostics()` wires this up), even if the
   Logger sink itself throws further, the `callback_boundary`'s own
   `try/catch + stderr fallback` (see L-31.6) is a second line of
   defence — two layers of protection, no path leads to
   `std::terminate`.
4. **Test coverage**: two contract tests in
   `runtime/tests/test_logger.cpp` ("throwing sink does not
   propagate (std::exception)" / "throwing sink does not propagate
   (non-std exception)") prevent regressions.

---

## 4. Call stack vs observation surface: when to throw, when to set

| Scenario | Path | Choice |
|---|---|---|
| Construction-time argument validation failure | Sync | **throw** `std::invalid_argument` |
| `Property::set` triggered cycle | Sync | **throw** `CircularDependencyError` |
| AsyncCommand body throws | Async (worker, then back to ui) | **set** `last_error` and **do NOT** rethrow to the user (`run_to_result_` folds the exception into `AsyncCommandResult::{Cancelled, Failed}`; `execute()` reports through `error_sink_`; `co_execute()` lets the caller branch on `r.failed()`) |
| AsyncResource fetch failure | as above | **set** `error` |
| Validator rule failure | Sync inside the graph | **set** `state.errors` |
| Cross-field rule failure | Sync inside the graph | **set** `first_error_full` |

**Core principle**:

- **Synchronous, caller is right there expecting a return value** →
  throw.
- **Asynchronous, caller has long since left** → set Property (the
  caller observes via subscription).
- **Cancellation** → neither throw nor set (it's control flow).

---

## 5. Anti-patterns

| # | Anti-pattern | Consequence | Correct approach |
|---|---|---|---|
| AE1 | `last_error.set(Error{kind=Cancellation, ...})` | UI displays "user cancellation" as an error | Cancellation does NOT go on the error surface; `classify_async_exception` already handles this correctly (folds into `AsyncCommandResult::Cancelled`, doesn't write `last_error`) |
| AE2 | Adapter setter throws `std::runtime_error` | Exception is swallowed by the dispatcher, UI fails silently | Don't throw from a setter; setters MUST be idempotent and non-throwing |
| AE3 | Constructing an Error with empty `source` and feeding it to a surface | The router can't tell which subsystem this came from | Every factory takes `source_tag` — the caller MUST provide it |
| AE4 | `last_error.get() == "kaboom"` to discriminate errors | String compare is fragile; messages may be localised | `last_error.get()->kind == ErrorKind::AsyncFailure` instead |
| AE5 | Letting `OperationCancelled` go through `Error::from_exception` | Becomes `AsyncFailure("operation cancelled")` on the observation surface | Catch `OperationCancelled` first in the catch chain and rethrow before `from_exception` sees it |
| AE6 | `Navigator::push(nullptr)` setting a Property instead of throwing | Caller proceeds; problem hidden | Synchronous paths MUST throw |

---

## 6. Cross-document references

- [`lifecycle.md`](./lifecycle.md) **L-21** spells out that Property
  writes are equality-gated; E-11 here depends on that to guarantee
  "rewriting the same error doesn't re-notify".
- [`api-style.md`](./api-style.md) **S-31** requires exception
  `what()` to follow the What/Where/How recipe; E-13 / E-20 here
  bring that recipe to the observation surface via `Error.kind /
  source / message`.
- [`api-style.md`](./api-style.md) **S-40** "no deprecated aliases":
  when this contract landed, every caller reading `last_error` as a
  string was **broken outright** and migrated to `last_error_message`
  or `last_error->message`.

---

## 7. P0-ε target mapping (TODO)

P0-ε fuzzers MUST verify these error-model invariants:

| Invariant | fuzzer |
|---|---|
| AsyncCommand cancellation never surfaces on the error face | `async_command_cancellation_no_error_fuzzer` |
| Repeated set of the same Error does not notify observers | `error_property_equality_gate_fuzzer` |
| Validator errors' `key.field_path` always equals the Validator's path | `validator_field_path_fuzzer` |
| `Error::from_exception` mapping is stable across std exception types | `error_from_exception_table_fuzzer` |

---

## 8. Document governance

Every error-model protocol change MUST flow as: doc change → code
change → test change. Any new `ErrorKind` MUST first be registered
in the E-1 table here.
