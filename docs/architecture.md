# Architecture

This document explains the design principles behind aria.

## Design goals

1. **One core, every platform.** Application logic should be portable from
   a console test, through Qt on the desktop, all the way to Android JNI and
   the WebAssembly browser sandbox — without `#ifdef`s.
2. **Pay only for what you use.** The header-only core has zero runtime
   dependencies. Pull in `runtime`/`binding` only if you need them; pull in
   a platform adapter only when targeting that platform.
3. **ABI stable.** Hide template implementations behind a small non-template
   "ABI" layer so internal optimisations don't force callers to recompile.
4. **Compile-time safety.** Concept-driven public API — the compiler refuses
   misuse with a one-line error rather than 200 lines of template gibberish.
5. **Predictable performance.** No hidden allocations on the hot path of
   `set()/get()`. The reactive graph is single-threaded and lock-free by
   design; signal-backed events (`ObservableList`, `EventBus`) fire
   observers outside their short critical sections.

## Layer model

```
                      Application code
                              │
            ┌─────────────────┼─────────────────┐
            ▼                 ▼                 ▼
        Adapters       Adapters           Adapters
       (Qt6/AppKit)   (JNI/UIKit)        (WASM/...)
            │                 │                 │
            └────────┬────────┴────────┬────────┘
                     ▼                 ▼
              ┌────────────┐    ┌─────────────┐
              │   binding  │    │   runtime   │  ← shared libs
              └─────┬──────┘    └──────┬──────┘
                    └─────────┬────────┘
                              ▼
                  ┌──────────────────┐
                  │   core / async   │   ← header-only templates
                  └─────────┬────────┘
                            ▼
                  ┌──────────────────┐
                  │       abi        │   ← static, ABI-stable
                  └──────────────────┘
```

### `aria-abi`

Pure non-template code: type-erased `SignalErased`, `SlotErased`, version
metadata. **Never** changes its ABI within a major version. Built as a
`STATIC` library so each consumer can link it without DLL boundary issues.

### `aria-core`

Header-only templates that build on a single **reactive dependency-graph**
engine (see `aria/reactive/`):

- `Property<T>` — a **source node**. Writes mark dependents dirty; reads
  auto-register the active evaluation (Computed / Effect) as a dependent.
- `Computed<T>` — a **derived node**. The body is a plain lambda with *no*
  explicit dependency list; every Property read inside is tracked
  automatically. Values are recomputed lazily on `get()`, cached until a
  real value change invalidates the cache.
- `Effect` — an **autorun observer**. Re-runs whenever any Property/Computed
  it read last time changes. Returns a `Subscription` for lifetime control.
- `Command<Args...>` — an invokable with a `CanExecute` predicate that
  itself plugs into the reactive graph, so `enabled` bindings update
  automatically as their dependencies change.
- `ObservableList<T>` — fine-grained list change stream
  (`Insert`/`Remove`/`ItemChanged`/`Reset`).
- `Subscription` — a RAII handle (internally `shared_ptr<void>` with a
  custom deleter) returned by every observer-adding API.
- `Validator<T>` — declarative rule chains that reuse the same engine.

The engine supports `reactive::batch([&]{ ... })` for grouping writes into
one notification sweep, and `reactive::untracked([&]{ ... })` for reads
that should *not* register a dependency edge.

### `aria-async`

C++20 coroutine support. Built around a minimal `Task<T>` (lazy, single-shot,
exception-safe), an `IExecutor` interface, and a `ThreadPoolExecutor`. The
`schedule_on(executor)` awaitable lets you hop between threads with
`co_await`.

### `aria-runtime`

Process-wide services compiled into **one** shared library:
- `EventBus` — type-erased pub/sub (loose coupling between view-models)
- `Container` — IoC singleton/transient/factory
- `Dispatcher` — abstract main-thread runner (with portable
  `SimpleDispatcher` for tests and console apps)
- `Logger` — leveled logging with pluggable sink

Putting these in a shared library guarantees one instance per process even
when the process loads multiple plug-ins.

### `aria-binding`

Translates between view-models (Property/Command) and platform widgets via
`IViewAdapter`. Each adapter implements a small set of read/write/observe
operations; `BindingEngine` glues them to bind one-way and two-way.

#### Adapter conformance & application wiring

Qt6, AppKit and UIKit are all **production-grade adapters**:

* All three share the same internal shape — a `pimpl` `Impl` struct,
  a per-view-per-kind `Bridge` cache (the per-`IView` map of native
  ObjC delegates / target-action wrappers), and `aria::abi::SignalErased`
  fan-out that lets every observer subscription detach via
  `disconnect_via_weak`. ARC retention of bridging objects is anchored
  on the `NSView*` / `UIView*` itself with `objc_setAssociatedObject`,
  so adapter-side state lives exactly as long as the native widget.
* `IView::~IView` calls `fire_destroy_()` while the native handle is
  still live, which gives `BindingEngine` a deterministic moment to
  drop the per-view subscription bucket before the storage is
  reclaimed.
* The `adapter_conformance` battery in
  `modules/binding/include/aria/binding/testing/adapter_conformance.hpp`
  is the single contract pinning all three adapters: 12 cases /
  25 assertions covering text / bool / int / double two-way, click,
  command + can_execute → enabled, view-destroy safety, and
  feedback-loop suppression for converter-based bindings. Platform
  conformance coverage belongs in tests rather than application demos.
* [AriaTools](https://github.com/dqsjqian/AriaTools) is the flagship
  cross-platform application for Qt, iOS, and Android (Web support is a
  work in progress). It owns end-to-end application wiring, while this
  repository keeps adapter contracts and minimal focused snippets.
* Saturating int64/uint64 → int narrowing for native widgets that
  top out at `int` (NSStepper, UIStepper, QSpinBox) lives in
  `modules/binding/include/aria/binding/detail/numeric_saturate.hpp`
  and is shared by all three adapters. On overflow the value is
  clamped to `[INT_MIN, INT_MAX]` and **at most one**
  `runtime::Logger::warn` entry is emitted per
  `(op_label, direction)` tuple per process (per-dylib in SHARED
  builds — adapter labels are namespaced `qt::*` / `appkit::*` /
  `uikit::*` so dedup never collides across dylibs). Direction is
  one of `over` / `under`; over- and under-flow on the same label
  carry independent budgets. A hard cap of 256 distinct keys
  protects against pathological label generation. Tests can call
  `aria::binding::detail::reset_saturate_warning_dedup_for_testing()`
  to re-arm the budget — production code must not.

#### HTTP adapter (`modules/adapters/http`)

The HTTP adapter (`aria::adapters::http::HttpAdapter`) is structurally
the same as Qt/AppKit/UIKit — same `IView` and `IViewAdapter` contract
— but the "native handle" it wraps is a logical string id, not a
widget pointer. State pushed through the `IViewAdapter` setter API
fans out via Server-Sent Events to every connected browser; inbound
REST requests dispatch back into the registered observer callbacks
the same way Qt's `editingFinished` does.

The implementation is intentionally compact: the HTTP/1.1 + SSE server
is provided by the vendored single-header [cpp-httplib]
(`third_party/cpp-httplib/httplib.h`, MIT) and all JSON encode/decode by
the vendored single-header [nlohmann::json]
(`third_party/nlohmann_json/`, MIT). Both are header-only and committed
into the tree, so enabling the adapter adds **no new external
build-system dependency** beyond what already ships in `third_party/`
(HTTPS additionally builds the vendored OpenSSL when
`ARIA_HTTP_ENABLE_TLS=ON`). What aria *owns* lives in a single
`http_adapter.cpp`: the wire protocol, the view registry + shadow state,
subscription dispatch, and the SSE fan-out across connected clients. The
browser side is a vanilla-ESM SDK
(`modules/adapters/http/web-sdk/aria_client.js`) that works in any
modern browser without a build step.

This adapter is the **right shape for "C++ stays on a server, browser
is a thin client"** — desktop apps with a web admin panel, headless
services with a debug UI, local debug dashboards. The (still-planned)
WASM adapter is reserved for the **opposite shape** — "C++ runs inside
the browser sandbox" — and is subject to hard sandbox constraints
(CORS, no native sockets, no thread preemption, OPFS-only storage)
that make it unsuitable for many real workloads. See
[RFC 0001](rfc/0001-http-adapter.md) for the protocol design.

## Subscription safety

A common bug in observer-based frameworks is calling `unsubscribe()` after
the publisher has been destroyed. aria prevents this by design:

```
Subscription  =  std::shared_ptr<void>   (with custom deleter)
```

The shared handle **owns its disconnect action**. Dropping the handle calls
the deleter, which itself only touches the publisher through a
`std::weak_ptr` — so if the publisher is already gone, disconnect is a
no-op. There is no type-erased pointer chasing and no risk of
use-after-free.

This is verified by the unit test `Property: subscription survives Property
destruction`.

## Threading model

aria is not monolithic in its concurrency story — different subsystems
make different trade-offs, and the division is deliberate:

### Reactive graph (`Property` / `Computed` / `Effect`) — single-threaded

The reactive engine runs on exactly **one** "graph thread", typically
the UI thread of your application.

- `get()` / `set()` / `mutate()` assert they are on the graph thread;
  debug builds fire an assertion on violation.
- No locks are taken on the hot path: reads and writes touch the graph
  directly, and `reactive::batch([&]{ ... })` coalesces nested writes
  into a single flush at the outermost boundary.
- `reactive::untracked([&]{ ... })` reads a value without registering
  a dependency edge.

To update a Property from a background thread, marshal through the
runtime's `Dispatcher`:

```cpp
pool.submit([dispatcher]{
    auto result = do_slow_work();
    dispatcher->post([result]{
        my_property = result;   // now on the graph thread
    });
});
```

The `aria-async` coroutine helpers make this even cleaner:

```cpp
co_await schedule_on(pool);               // background work
auto r = compute_heavy();
co_await schedule_on(main_dispatcher);    // hop to UI / graph thread
my_property = r;
```

### Fire-and-forget signals — thread-safe

(`ObservableList`, `EventBus`, Command's `CanExecuteSignal`.)

These live outside the reactive graph and have always been free to
cross threads. Their internal `abi::SignalErased` takes a brief
unique-lock for emit/connect/disconnect, but **observer callbacks are
invoked without holding any publisher lock**, so handlers can touch
the publisher (or any other signal) without deadlock.

> **Command predicate still runs on the graph thread.** The
> `CanExecuteSignal` itself is thread-safe and can be observed from
> anywhere, but the predicate installed in a `Command<>` reads
> `Property` / `Computed` values, so the internal `Effect` that
> auto-tracks it must fire on the graph thread. In practice this is
> automatic: the reactive graph is single-threaded, so the Effect
> runs wherever writes to its upstream Properties happen — which is
> exactly where the docs require them to happen.

`ObservableList<T>` in particular uses `std::shared_mutex` internally
so snapshots, lookups, and mutations are safe from any thread. It is
deliberately **not** a reactive-graph node: a collection-level change
notification is coarser than the per-value versioning the graph uses,
and the lock-based model composes naturally with the async runtime.

## ABI stability scope

The "ABI stable" promise in the README applies to a specific set of
boundaries, not to every type in the repo:

| Layer / symbol                                 | ABI status |
|------------------------------------------------|-----------|
| `aria-abi` (`SignalErased`, version metadata)  | stable within a major version |
| `aria-runtime` non-template exports (`EventBus`, `Container`, `Dispatcher`, `Logger`) | stable within a major version |
| `aria-binding` non-template exports (`BindingEngine` type-erased parts, `IViewAdapter` vtable) | stable within a major version |
| `aria-core` templates (`Property<T>`, `Computed<T>`, `Command<...>`, `ObservableList<T>`) | **source-compatible**, not binary-stable — they are instantiated in the user's TU |
| `IView` base class (data members `destroy_signal_`, `fired_`) | tied to `aria-binding`; adapters recompiled with a new binding release are expected |

In other words: if you ship a plug-in that only talks through
`aria-runtime` / `aria-binding` **vtables** and ABI handles, it can
continue to work across minor upgrades. If you ship a plug-in that
inlines `Property<T>` or derives from `IView` in its own DLL, treat
every minor release as a rebuild boundary.

## Memory ownership

The ownership rules divide along the same lines as the threading model.

### Reactive graph (`Property`, `Computed`, `Effect`)

Dependencies between nodes are held as **intrusive doubly-linked edges**
(`reactive::Edge`), not as reference-counted handles. A `Subscription`
returned from `Property::on_changed` / `Effect::into_subscription` /
`Computed::bind` is a `shared_ptr` to the Reaction node; dropping the
handle destroys the node, which in turn detaches every edge in O(deps).
Because edges are intrusive, the publisher never owns the subscriber and
vice-versa — there are no reference cycles to leak.

### Signal-backed events (`ObservableList`, `EventBus`, `Command::CanExecuteSignal`)

These use the ABI-stable `abi::SignalErased`, whose control block is a
`shared_ptr`. A `Subscription` keeps only a `weak_ptr` to that control
block:

- If the publisher is still alive when the Subscription is dropped, the
  slot is removed from the signal.
- If the publisher is already gone, the weak lock fails and disconnect is
  a silent no-op — no use-after-free.

`ObservableList<T>` in particular holds `shared_ptr<T>` for its elements
and creates per-item subscriptions whose handlers also go through the
same weak-handle dance, so a destroyed list never leaks dangling slots
on its items.

Taken together these rules guarantee:

- **No cycles** — subscriptions never own the publisher.
- **No use-after-free** — disconnect is always weak-guarded.
- **No races on the control block** — refcounting is intrinsically
  thread-safe.

### Binding-engine lifetimes

`BindingEngine` sits between the ViewModel side (`Property`, `Command`)
and the platform view side (`IView`). It takes responsibility for one
of those two sides only:

- **View lifetime is handled by the engine.** Every `bind_*` call
  routes its subscriptions through a per-view bucket; when
  `IView::on_destroy` fires, the bucket is cleared and the map entry
  erased, so no later `prop.on_changed` callback ever dereferences a
  dead view. Destroying the engine releases every bucket in one shot.
- **Source lifetime is handled by the caller.** The engine captures
  `Property` and `Command` by reference, and expects the owning
  ViewModel (or whatever scope holds them) to outlive the bindings.
  The idiomatic shape is that the engine is a member of a `ViewModel`
  or a sibling scope object, so both sides die together. If you need
  the Property / Command to outlive the engine, call `engine.clear()`
  (or destroy the engine) before releasing them.

## Future work

- **Swift adapter** (`modules/adapters/swift`): bridge Aria to
  Swift/SwiftUI via C++ Interop (5.9+). See ROADMAP P2-II.b.
- **WASM adapter**: bridge Aria to emscripten + JS-side reactive
  systems. See ROADMAP P2-II.c.
- **Reflection-based auto-binding** when C++23 `std::meta` becomes widely
  available.

## ABI Stability Policy

Aria is split across modules with deliberately different stability
contracts. The short version:

| Module | Surface | Stability |
|---|---|---|
| `aria-abi`     | C-style header (`aria/abi/*.hpp`), no templates | **True ABI**: stable across minor versions, breaks only on a major bump. The macro `ARIA_ABI_VERSION` is the source of truth. |
| `aria-runtime` | Non-template runtime services (Container, Dispatcher, Logger, EventBus glue) | **True ABI**: stable like `aria-abi`. The dynamic library can be replaced under a consumer at install time. |
| `aria-binding` | `BindingEngine`, `IViewAdapter`, `IView` (non-template) | **True ABI**: stable like `aria-abi` / `aria-runtime`. |
| `aria-core`    | `Property<T>`, `Computed<T>`, `Command<...>`, `ObservableList<T>`, `FilteredList<T>`, `SortedList<T>`, `MappedList<S,T>`, reactive graph internals | **Source-only**: every header is compiled into the consumer's translation unit; the symbols emitted are mangled by `T` and live in the consumer binary. *No* binary compatibility across builds is offered or required. |
| `aria-async`   | `Task<T>`, `AsyncCommand<R, Args...>`, `with_timeout`, `retry*`, `CancellationToken` | **Source-only**, same reasoning. |
| `aria-adapters-*` | Qt6 / AppKit / UIKit glue | **Source-only** at the moment. AppKit / UIKit modules ship as INTERFACE / OBJECT targets (compiled inside the consumer Xcode app); all three adapters are production-grade and pinned under the same `adapter_conformance` battery. A future packaging change may move them behind `aria-binding`'s ABI line once Windows DLL parity is needed. |

### What "ABI stable" means concretely

A consumer that links against `libaria_runtime.X.dylib` can be
upgraded to `libaria_runtime.Y.dylib` (where `X` and `Y` share a
major version) without recompiling, provided:

* No virtual function in an exported class has changed signature or
  ordering.
* No struct exported by an `aria-abi` / `aria-runtime` /
  `aria-binding` header has changed layout.
* No symbol previously exported has been removed.

How this is checked today
------------------------

There is **no** `abi-dump` job. An earlier revision of this document claimed
one existed on tagged releases; it never did, and `scripts/` contains no such
tool. What CI actually runs is the `abi-smoke` job: it builds with
`ARIA_BUILD_SHARED=ON` and executes `cross_dylib_abi_smoke`, which exercises
`IProperty<T>` across a real dylib boundary. That catches gross breakage
(missing symbols, vtable mismatch on the smoked interfaces) but does not
diff exported symbol sets between releases.

For the runtime half, `aria::abi::runtime_abi_version()` and
`runtime_version_string()` are compiled into the library, so a host that
loads aria dynamically can assert agreement with its headers at startup via
`aria::abi::abi_matches_headers()`. That is the enforceable part; a genuine
symbol-set diff on tagged releases remains unimplemented, and the guarantee
above should be read as a maintenance intent rather than a verified property.

The layout guarantee also assumes a consumer built with the same compiler and
standard library as the library itself — `IViewAdapter`'s virtual signatures
pass `std::string` and `std::function` by value, so a mixed-stdlib link is
outside the promise regardless of ABI version.

Source-only modules (`aria-core`, `aria-async`, `aria-adapters-*`) are exempt
— their symbols recompile inside each consumer.

### When you SHOULD care

* Building Aria as a shared library and shipping the .dylib /
  .so / .dll separately from the application binary.
* Plug-ins that load adapter implementations at runtime via
  `dlopen`. (Plug-in surface = `IViewAdapter` + the runtime / abi
  modules. The plug-in itself can use `aria-core` freely *inside*
  its own translation unit; that does not cross the ABI boundary.)

### When you SHOULD NOT care

* Static linking the entire framework into a single executable.
* Statically linking `aria-core` for `Property<MyDomainType>` —
  the compiler will inline and template-instantiate everything in
  your TU; "ABI" is irrelevant.

### IProperty

A type-erased `IProperty` virtual interface (`get_any` / `set_any` /
`subscribe_any` / `type`) lets plug-ins observe and mutate
`Property<T>` instances without sharing the concrete `T`.
`aria::Property<T>` inherits from `aria::IProperty` and
implements all four virtuals; the cost is one virtual call plus a
`std::any` round-trip per access.

The cost numbers (Apple Silicon, -O3, single-threaded
`bench_iproperty`):

| Operation                              | ns/op |
|----------------------------------------|-------|
| `Property<int>::peek()`                | ~0.3  |
| `Property<int>::get()`                 | ~1.4  |
| `IProperty::get_any() + any_cast<int>` | ~6.0  |
| `Property<int>::set(i)`                | ~180  |
| `IProperty::set_any(any{i})`           | ~187  |

Type erasure adds ~5-7 ns over the template path on get / set for
trivially-copyable payloads (the int payload fits in `std::any`'s
SBO). Larger payload types pay for an additional heap allocation
when `std::any` spills out of its SBO. Either way, template-direct
should still be preferred in hot paths inside the host module;
plug-in / RPC / live-binding boundaries are the right place to pay
the type-erasure cost.

A cross-dylib smoke test (load a plug-in and operate on its
`IProperty*` from the host) is **not** shipped today. It is gated
on a real plug-in driver appearing — without one, building the
necessary CI matrix to validate libc++ ↔ libstdc++ ABI parity is
all cost and no signal. The interface is fully usable inside a
single binary today; cross-dylib will land when there is a real
customer for it.

The header lives at `modules/core/include/aria/i_property.hpp`;
the implementation is inline in
`modules/core/include/aria/reactive/property.hpp`; the bench is
`benchmark/bench_iproperty.cpp`; the tests are
`modules/core/tests/test_i_property.cpp`.

## Coroutine race model

`aria-async` ships several awaitable combinators that resolve the
parent coroutine from one of multiple signaller paths:

| Combinator | Signallers |
|---|---|
| `with_timeout(..., OnTimeout::Cancel)` | inner coroutine (cooperative; deadline only flips a token) |
| `with_timeout(..., OnTimeout::Fail)`   | inner coroutine **vs** deadline timer |
| `when_any(...)`                        | N input tasks, first wins |
| `when_any_cancellable(...)`            | N factories receiving tokens; winner cancels losers |

All race-style combinators share a single primitive — `detail::RaceSlot<R>`
in `modules/async/include/aria/async/detail/race_slot.hpp` — and a
**two-phase publication protocol** (hardened after a peer-review
found a publication race in the original single-atomic design):

1. **Phase 1 — CLAIM (writer-resource CAS).** Each signaller calls
   `slot.try_claim(code)`; the CAS targets a private `claimed_`
   atomic that the parent NEVER observes. Only the first signaller
   to flip it wins the writer slot. Late signallers silently drop.
2. **Phase 2 — WRITE then PUBLISH (winner-only, ordered).** The
   winner populates `slot.result` (a
   `std::variant<monostate, value, exception_ptr>`) and
   `slot.winner_index`, **then** calls `slot.publish(code)`, which
   release-stores the same code into the parent-observed `winner_`
   atomic. The parent's `await_ready` / `await_suspend` acquire-load
   `winner_`; observing a non-zero value synchronises-with the
   release-store and guarantees `result` is fully written.
3. **Phase 3 — RESUME (mu_-serialised handle handoff).** The winner
   finally calls `notify_winner_resume()`, which under `mu_` reads
   `parent_handle` and resumes (or no-ops if the parent has not yet
   stored its handle — the parent's `await_suspend` tail re-checks
   `winner_` under `mu_` and skips suspension).

Why two atomics instead of one: the original protocol stored the
winner code BEFORE `result` was written, so a parent observing
`winner_ != 0` via `await_ready()` (which does NOT take `mu_`) could
rush into `await_resume()` and read an unpublished `result` — UB.
Splitting into "claim" (writer-resource lock, internal) and "winner /
published" (release-store AFTER result is written, parent-observed
under acquire) closes the window without changing the public awaiter
API.

`mu_` does NOT serialise result publication — that is handled by
acquire/release on `winner_`. `mu_` serialises the orthogonal
handle-handoff race so exactly one of {publisher.resume,
parent.skip-suspend} fires.

`await_suspend` returns `false` (skip suspension) whenever it
re-checks `winner_` under `mu_` and finds the slot already published.
This is the standard C++ coroutine-protocol opt-out for "the work
is already done; do not park me".

### `when_all` is NOT race-style

`when_all` waits for ALL inputs to complete and lacks a winner CAS.
Its `await_suspend` publishes the parent handle through an atomic
(`detail::WhenAllParentHandle`, a `std::atomic<void*>` with
release-store / acquire-load) *before* spawning any driver. Drivers
load the handle only after the `remaining_` atomic counter hits zero,
so even a synchronously-completing single-task `when_all(...)` observes
the published handle. Because the handle now lives in an atomic, the
handoff is TSan-clean and its correctness no longer depends on the
ordering of the two halves of `await_suspend` — the store-before-spawn
order is retained purely as an optimisation.

### What this primitive does NOT solve

* **Non-cooperative inner work in `OnTimeout::Fail`**. The inner
  coroutine continues running detached after the timer wins.
  RaceSlot guarantees the parent is unblocked promptly and the
  late inner result is not delivered, but it cannot kill an
  uncooperative inner — that is a fundamentally cooperative
  property of the inner work.
* **`OnTimeout::Cancel` + no-token factory is observe-only.** When
  the factory does not accept a `CancellationToken`, the timer has
  no channel to ask the inner work to stop. Combined with the
  default `OnTimeout::Cancel`, the deadline can only OBSERVE the
  inner outcome and re-label it as `TimeoutError` AFTER the inner
  work finishes naturally — the caller does NOT get a prompt
  timeout. For real fail-fast deadlines, either pass a
  token-accepting factory (so the inner can cooperatively unwind)
  or use `OnTimeout::Fail`. This combination is preserved as a
  deliberate "best-effort observe" mode and is documented at the
  top of `aria/async/timeout.hpp`.
* **`when_all` parent-handle handoff** is now an atomic
  (`std::atomic<void*>` in `detail::WhenAllParentHandle`,
  release-store / acquire-load), so `when_all` IS TSan-clean and a
  candidate for TSan stress. The ordering invariant is enforced by the
  atomic rather than by program-logic statement ordering.

### Tracking

The race model is implemented as a single shared `RaceSlot<R>`
primitive plus the two-phase publication protocol described above.
All race-style awaiter `await_suspend` paths are audited for the
publication-window hazard and covered by `test_race_slot_publish`
plus the TSan stress harness.
