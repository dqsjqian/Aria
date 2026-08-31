# Aria Lifecycle and Threading Model

> This document is the framework's authoritative reference for
> **lifecycle and threading**. When any API comment says "see the
> lifecycle doc", it means this file.
> The document is split along two axes:
> - **Threading contract**: which thread can call what, what actions
>   cross threads, and the legal cross-thread paths.
> - **Lifetime contract**: object construction/destruction order,
>   subscription detach timing, and callback visibility boundaries.
>
> Every contract item has a **unique number** (`L-N`) for citation in
> code, tests, and the CHANGELOG. The "MUST / MUST NOT / MAY" wording
> follows RFC 2119 conventions.

---

## 0. Overview

Aria's concurrency model has a single load-bearing axis: **the
reactive graph is pinned to a single thread**; every other subsystem
interacts with the graph thread through an explicit protocol.

```
                ┌──────────────────────────────┐
worker thread   │   ThreadPool / IO / etc.     │
                └────────────┬─────────────────┘
                             │ co_await schedule_on(ui)
                             ▼
                ┌──────────────────────────────┐
graph thread    │   reactive::Graph (single)   │
(= UI thread)   │   Property / Computed /      │
                │   Effect / Binding / VM      │
                │   ObservableList writes      │  <- core converges here
                │   AsyncCommand Property write │
                └────────────┬─────────────────┘
                             │ post / post_after
                             ▼
                ┌──────────────────────────────┐
delayed timer   │   IDelayedScheduler (UI/VT)  │
                └──────────────────────────────┘
```

Anything that violates this model is listed in the "Anti-patterns"
section. Any user code that finds itself in an anti-pattern position
has **broken the contract**; the framework does not promise
well-defined behaviour.

---

## 1. Threading contract

### L-1: Graph thread affinity

`reactive::Graph` is a process-wide singleton. **Every public method
on a `Graph` instance MUST be called from the same thread.**
That thread is called the **graph thread**, implicitly chosen by the
first thread that touches a public method, and every subsequent call
MUST come from the same thread.

`Graph::assert_on_graph_thread()` checks via `assert` in Debug
builds; Release builds are zero-overhead.

**Scope**: every operation below is a graph-thread action:
- `Property<T>::set / operator= / mutate`
- `Property<T>::get / peek` (reads do not fail, but the `tracker` is
  graph-thread bound)
- `Property<T>::on_changed / bind / observe`
- `Computed<T>` construction, reading, subscription
- `Effect` construction and destruction
- `reactive::batch / untracked / BatchScope / UntrackedScope`
- Destruction of `Subscription` handles backed by a Reaction node
  (subscriptions returned from Property/Computed; their destruction
  hits the graph, so it counts as a graph-thread action)

**Anti-pattern**: calling `prop.set(x)` from a worker thread.
**Legal path**: prepare data on the worker, then
`co_await schedule_on(ui_executor)` to hop to the graph thread, and
write the Property there.

### L-2: the only legal cross-thread path

The single legal path for triggering a graph mutation from another
thread is: **post a callback to the graph thread via any
`aria::IScheduler::schedule`** (including the
`IDispatcher::post` / `IExecutor::post` aliases) and call graph APIs
from inside that callback once it resumes on the graph thread.

`AsyncCommand` and `BindingEngine` (`SmartMarshal` / `AlwaysPost`
policies) internalise this path — `prop = result;` inside a
coroutine works without a manual `post` provided the previous
`co_await schedule_on(ui)` has completed.

### L-3: reactive Graph and `abi::Signal` use different threading models

| Subsystem | Threading model | Lock |
|---|---|---|
| `reactive::Graph` (Property / Computed / Effect) | Single thread (graph thread) | none |
| `abi::SignalErased` (ObservableList, Command::can_execute, EventBus, IView::on_destroy) | Multi-thread safe | `std::mutex` (snapshot then release on emit) |
| `ObservableList<T>` structural mutations | Multi-thread safe | `std::shared_mutex` |
| `IDispatcher` / `IExecutor` / `IDelayedScheduler` (all `IScheduler`) | Multi-thread safe | implementation-private |

**Implications**:
- The reactive subsystem requires no user-side locking but the user
  **MUST** ensure all calls are made on the graph thread.
- `Signal` / `ObservableList` / `EventBus` are callable across
  threads, but **the final hop into a Property MUST happen on the
  graph thread** (see L-1).

### L-4: BindingEngine has three dispatch policies

`BindingEngine` provides three VM→View routing policies. The
**default is `Direct`** (zero overhead, assumes single-threaded
MVVM). Production-recommended is `SmartMarshal`:

| Policy | Behaviour | Use case |
|---|---|---|
| `Direct` | Inline call, assumes the emit is already on the UI thread | Pure single-threaded; tests |
| `SmartMarshal` | Inline if `dispatcher.is_main_thread()` is true, otherwise `post` to the main thread | Production; coexists with a worker pool |
| `AlwaysPost` | Always `post` to the dispatcher | Tests that need deterministic "emit→post→update" ordering |

**View→VM is never marshalled**: native callbacks already fire on
the UI thread.

### L-5: AsyncCommand write-back thread contract

`AsyncCommand` MUST take a graph-safe executor as its `ui` argument
(`is_safe_graph_executor_v` checks at compile time); its internal
coroutine template looks like:

```
schedule_on(ui)  ->  schedule_on(worker)  ->  user action  ->  schedule_on(ui)  ->  write Property
```

**The final `schedule_on(ui)` is the contractual guarantee for the
command's Property write-back.**
If the user passes `InlineExecutor` as `ui` while the worker is not
inline, the compile-time `static_assert` and the runtime
`check_executor_safety_runtime` both reject it.

### L-5b: Startup ordering — executors and timers before view models

**Platform executors and delayed schedulers MUST be installed before
any `AsyncCommand`-owning view model is constructed.**

`AsyncCommand` validates its executors in its constructor, not on first
execution. Constructing a view model that owns one before a real
main-thread `IExecutor` exists therefore throws `std::invalid_argument`
at construction time:

> `AsyncCommand: cannot use InlineExecutor as the graph-thread executor
> when worker runs on a different thread. Remedy: install a real
> main-thread executor BEFORE constructing this view model …`

Correct host startup order:

1. create the platform dispatcher (`QtDispatcher`, `SimpleDispatcher`, …);
2. wrap it for the async layer — `aria::runtime::DispatcherExecutor` for
   `IExecutor`, `aria::runtime::DispatcherScheduler` for
   `IDelayedScheduler` (both in `aria/runtime/dispatcher_executor.hpp`);
3. construct view models, passing those executors in;
4. build views and bind.

```cpp
// Order of member declaration = order of construction in the host shell.
std::shared_ptr<QtDispatcher> dispatcher;   // 1  platform dispatcher
runtime::DispatcherExecutor   ui_exec;      // 2  IExecutor  view of it
runtime::DispatcherScheduler  delay;        // 2  IDelayedScheduler view of it
async::ThreadPoolExecutor     worker;

// 3  view models — constructed only after the executors above exist
vm_login  = std::make_shared<LoginVm>(ui_exec, worker);   // owns AsyncCommand
vm_search = std::make_shared<SearchVm>(delay);            // owns debounce

// 4  views + bindings
auto* view = build_view(*vm_login, engine);
```

In tests and console applications `MainThreadExecutor` plays the role of
step 2 — it is already `GraphSafe | MainThread | Pumpable`.

Aria states this ordering contract; the host enforces it. There is
deliberately no bootstrap/orchestrator class: the startup sequence a real
application needs also covers module loading and view registration,
neither of which Aria owns.

### L-6: IScheduler / IDelayedScheduler timing contract

Every scheduler abstraction in the framework virtually inherits from
`aria::IScheduler`:

```
IScheduler
  ├─ caps()              -> SchedulerCaps                  // capability bitmask
  ├─ schedule(fn)        -> void                            // Caps::Post
  ├─ schedule_after(...) -> void  (default throws)            // Caps::Delay
  └─ is_main_thread()    -> bool  (default false)             // Caps::MainThread
```

A caller checks capability in one line:
`if (has_caps(s, SchedulerCaps::Delay)) ...`. Where degradation is
not acceptable, `require_caps(s, ..., "context")` throws
`unsupported_capability`. Concrete capability sets:

| Concrete | caps() |
|---|---|
| `InlineExecutor`         | `Post \| GraphSafe \| WorkerSafe` |
| `ThreadPoolExecutor`     | `Post \| WorkerSafe \| Autonomous` |
| `MainThreadExecutor`     | `Post \| GraphSafe \| WorkerSafe \| MainThread \| Pumpable` |
| `VirtualTimeExecutor`    | `Post \| Delay \| GraphSafe \| WorkerSafe \| Pumpable` |
| `SimpleDispatcher`       | `Post \| Delay \| MainThread \| Pumpable` |
| `QtDispatcher`           | `Post \| Delay \| MainThread \| Autonomous` |

`IDelayedScheduler::post_after(delay, fn)` (alias of
`schedule_after`) promises:
- `fn` is NOT invoked while `now < dispatch_time + delay`.
- `fn` is invoked **as soon as possible** once `delay` elapses, but
  **not necessarily immediately** — implementations MAY coalesce
  adjacent timers.
- Under `SimpleDispatcher`, `fn` runs on the thread that owns the
  dispatcher; the caller MUST ensure that thread is the graph
  thread (or `post` to the graph thread from inside `fn`).
- Under `VirtualTimeExecutor`, time is advanced explicitly via
  `advance_by` / `run_until_idle`; `fn` runs synchronously on the
  thread that called `advance_by`.

---

## 2. Lifetime contract

### L-10: Subscription destruction semantics

`aria::Subscription` is the unified RAII detach handle: **move-only,
not copyable**.

- Destruction (or `release()` / `detach()`) triggers detach **exactly
  once**.
- "Detach" means different things for the two backends:
  - **Reactive-backed** (from `Property::on_changed` /
    `Computed::bind` / `Effect`): destruction releases the
    `ReactionNode`; the node's destructor runs `clear_sources()` to
    detach upstream edges.
  - **Signal-backed** (from `TypedSignal::connect` /
    `IView::on_destroy` / `ObservableList::observe`): destruction
    invokes a `disconnect-via-weak` callback that removes the slot
    from the signal's entries list.

### L-11: reactive-backed Subscription destruction MUST be on the graph thread

A reactive-backed `Subscription` owns
`std::shared_ptr<ReactionNode>`. Releasing the node triggers
`Node::~Node` → `clear_sources()` → `detach_edge`, all of which are
graph operations (L-1 applies).

**Therefore**: subscriptions returned by the reactive subsystem
(including those moved out of `Effect::into_subscription()`) MUST be
destroyed on the graph thread.

> **Current state**: this contract is implicit in the common pattern
> "the VM drops its SubscriptionBag in its destructor" — VMs
> typically live on the graph thread.
> But there is no explicit `assert` for the destruction thread of a
> reactive-backed handle. **P0-ε MUST add a stress fuzzer for it**
> (an "observer-destroy-from-wrong-thread" fuzzer).

Signal-backed `Subscription` destruction is lock-protected and
multi-thread safe.

### L-12: SubscriptionBag destruction order

`SubscriptionBag` is a `std::vector<Subscription>`. Destruction
order follows `std::vector::~vector` semantics — **destroyed in
reverse construction order**. Implications:
- The Subscription added later detaches first.
- If the user requires "detach A before detach B", express that
  through the `+=` order.

### L-13: unsubscribing during emit is allowed

Every `abi::SignalErased`-backed emit (`ObservableList`, `EventBus`,
`Command::can_execute_changed`, `IView::on_destroy`, `TypedSignal`)
uses **snapshot-then-invoke**:

```
emit:
  snapshot = entries.copy()    // under mutex
  release mutex
  for slot in snapshot: slot.invoke(args)
```

Implications:
- A slot in the snapshot will be invoked **even if `disconnect()`
  was called during this emit**.
- A slot newly registered **during** this emit will **not** be
  invoked.
- It is **safe and common** to `release()` your own Subscription
  inside a slot callback.
- It is **safe** to `release()` someone else's Subscription inside
  a slot callback (only affects subsequent emits).

**Reactive backend differs**: the graph processes via `pending_` in
topological order. See L-20.

### L-14: emitting the same signal recursively from inside emit

Allowed, but the user must avoid infinite recursion. `emit` does not
hold the lock; recursive emit is just another snapshot-then-invoke.

### L-15: destroying the signal itself during emit

`abi::SignalErased` holds its control block via `shared_ptr`; during
emit the snapshot keeps strong `shared_ptr<SlotErased>` references,
so **destroying the original signal during emit is safe** — the
snapshot keeps everything alive until emit completes, after which
the control block is released.

`disconnect_via_weak` is a no-op when the control block is gone.

### L-16: a reactive Node destructor MUST detach every edge

`Node::~Node()` strictly follows "detach downstream observers
first, then `clear_sources()` upstream"; otherwise dangling Edge
references to freed memory are left behind.

If a derived class (`Property` / `AutoComputed` /
`AutoReactionNode` / `ReactionNode`) holds members like
`std::vector<std::unique_ptr<Edge>>`, **the derived destructor MUST
call `clear_sources()` first** — C++ destruction order ("derived
members first, base last") would otherwise let `~Node` access freed
Edge storage when it calls `clear_sources()`.

> Every existing derived class (incl. detail::ReactionNode /
> AutoReactionNode / AutoComputed) calls `clear_sources()` in its
> own destructor and complies with the contract.

### L-17: when a Computed releases dynamic dependencies

`AutoComputed` / `AutoReactionNode` start each `recompute()` by
`clear_sources(); edges_.clear();`, then collect a fresh dependency
set under TrackerScope and call `attach_as_observer_of` to rewire.

Implications:
- "Ghost subscriptions" cannot exist — any upstream that was NOT
  read in the most recent recompute is no longer an upstream.
- "Dynamic dependencies" (conditional reads) work correctly: after
  a branch flip, the old branch's source disconnects automatically.
- Sources NOT touched by `dep` during recompute are NOT upstreams,
  even if they were previously.

**Anti-pattern**: capturing a reference to a source inside the
recompute body and reading it AFTER recompute returns — that read
registers on the outer tracker (if any), not on this Computed.

### L-18: Effect runs eagerly once on construction

`Effect::Effect(Fn)` immediately invokes `fn` once to gather the
initial dependency set (consistent with MobX `autorun` and
SolidJS `createEffect`).
If `fn` throws on the first run, `Effect`'s constructor throws —
the node was constructed but never assigned to `Effect::node_`, so
no orphaned node remains.

### L-19: Property::on_changed does NOT fire on the initial value; Property::bind does

| API | First-fire behaviour | Subsequent |
|---|---|---|
| `prop.on_changed(fn)` | Does NOT call `fn` | Calls `fn` on every actual change |
| `prop.bind(fn)` | Synchronously calls `fn(value)` once | Same as on_changed |
| `prop.observe(fn)` | Does NOT call | Calls `fn(old, new)` on every change |
| `Computed::on_changed(fn)` | Does NOT call (suppressed via a "primed" flag) | Calls `fn` on every actual change to the computed value |
| `Computed::bind(fn)` | Synchronously calls `fn(get())` once (inside the graph) | Same as on_changed |

**Note**: `Computed::bind`'s initial call happens inside the graph,
but is **not auto-wrapped in a batch** — if the user triggers a
`prop.set` chain inside the initial bind, they must wrap it in
`batch` themselves.

### L-20: subscription timing during a reactive flush

`Graph::flush` is non-reentrant: once flush starts, `flushing_ =
true`. While flushing, `Property::set` does NOT trigger another
flush; the change goes into `pending_` for the current flush's
next round.

Implications:
- `prop.set` inside an `Effect` body or `Computed::recompute` body
  is **allowed** — it gets processed in the next round.
- **Self-set never forms a cycle** — a subtle but critical
  invariant. The first thing every `Effect::recompute` does is
  `clear_sources()`: upstream edges are detached BEFORE `fn` runs.
  Inside `fn`, `set(p)` triggers `notify_changed`, but
  `p.observers_head_` is empty at that point (this Effect already
  detached itself), so this Effect does not re-enqueue into
  `pending_`. After `fn` returns, TrackerScope collects the
  freshly-read sources and rebuilds the edges. Result: a
  self-setting Effect converges naturally instead of looping.
- If flush exceeds `kMaxFlushRounds = 100` without converging →
  `CircularDependencyError` with the names of the still-pending
  nodes (up to 16). This safety net targets **real multi-node
  cycles** (A writes B, B writes C, C writes A) — not self-set.
- Destroying a reactive Subscription during flush: the Reaction
  node fires `clear_sources` to detach, but is **not** removed
  from the current round's vector / `pending_` — its `recompute()`
  may still be invoked in this round.
  `AutoReactionNode::recompute()` is `clear_sources();
  edges_.clear(); ... fn();`; if the node is dead, `fn` is
  effectively empty (technically `fn_` is a `std::function` and
  destructs after the node, so it still exists during emit).
  > **Implicit contract**: "destroy a subscription during flush"
  > on the reactive side is NOT as clean as on the signal side —
  > nodes are reference-counted via `shared_ptr` while the round
  > vector holds raw pointers. **Users MUST NOT destroy the
  > Reaction node currently being invoked from inside its own
  > Effect / Computed body.**

### L-21: Property writes are equality-gated

`Property<T>::set(v)` is a no-op when `value_ == v`: no version
bump, no push-color, no notification.
Implications:
- Idempotent writes are zero cost.
- The "looks unchanged but still fires" problem only occurs when
  the user's `T` doesn't define a strict `operator==`. P0-δ encodes
  "PropertyValue must be EqualityComparable" into the concept.

`mutate(fn)` does NOT do the equality check — it always fires.
Designed for container-typed Properties.

### L-22: Computed caching and version-based skipping

`Computed<T>::recompute()` does NOT bump `version_` when the new
value equals the cached value; downstream observers are not
notified.
`Graph::pull`'s MaybeDirty fast path: "every upstream's
`observed_version == source.version()`" → mark Clean without
calling recompute. This is the key to a glitch-free graph.

---

## 3. Subsystem contracts

### L-30: Property → ObservableList nested subscriptions

`ObservableList<T>` automatically installs a per-item subscription
when `T` satisfies `t.on_changed(fn)`. **The install step happens
AFTER the write lock is released** — so if `T` is a `Property`
(whose `bind`-style semantics fire once synchronously on subscribe),
the callback that calls back into `index_of_raw_` (which takes a
`shared_lock`) will **NOT deadlock** with the just-released write
lock.

**This is the load-bearing invariant for list-reactive interop.**
Any future change to `ObservableList` MUST preserve the rule
"install subscriptions outside the write lock".

### L-31: ObservableList emit ordering

`Insert` / `Remove` / `Replace` / `Move` / `Reset` / `ItemChanged`
all multicast through `abi::SignalErased`, following L-13.
For batch operations such as `insert_range` /
`remove_range` / `remove_all`, multiple single-element events are
emitted; **each event's `index` reflects the list's state as
observed at THAT emit**.

**Anti-pattern**: an observer assumes "list size = idx + 1" upon
receiving `Insert(idx=2)` — wrong, the list may already be larger.

### L-31.5: derived-list owning callbacks must be zero-allocation

The five derived-list owning callback types
(`FilteredList<T>::Predicate` / `SortedList<T>::Comparator` /
`MappedList<S,T>::Mapper` / `DistinctList<T,K>::KeyOf` /
`GroupedList<T,K>::KeyOf`) all use
`aria::inplace_function<…, 32>`; **capacity overflow is a
compile-time `static_assert`**. The "derived-list hot-path callback
never triggers malloc" property is type-system enforced, NOT a
documentation promise.

**Contract**:
1. The lambda capture of these callbacks must be ≤ 32 bytes total
   with alignment ≤ `alignof(max_align_t)`. Common shapes (`[this]`
   / `[a,b,c]` / `[&,n]`) all fit. If a workload needs more capture,
   either: (a) wrap the state in a `shared_ptr` and capture the
   single pointer, or (b) define a custom derived list on the
   business side using a larger `inplace_function<…, M>`.
2. A `static_assert` failure here is a contract violation, not a
   bug. We will not bypass it via `std::function`, nor will we
   "raise the default 32" to paper over individual offenders.
3. `inplace_function` is **copyable** when the erased callable is
   copyable (copy goes through `Op::CopyConstruct`); **movable**
   when the erased callable is movable. A move-only lambda fails to
   compile at the copy-construction site — an upgrade equivalent
   to `std::function`'s behaviour, with zero caller change.
4. `function_ref` does NOT play in the owning lane. If anyone
   accidentally uses `function_ref` as a derived-list storage
   field,    follow the "hot-path callable contract" section in
   [api-style.md](api-style.md) and switch back to `inplace_function`.

### L-31.6: unified callback-failure reporting (callback boundary)

Every framework-internal "must stay `noexcept` yet calls into a
user callback" boundary
(`ThreadPoolExecutor::worker_loop_` / `MainThreadExecutor::drain` /
`MainThreadExecutor::run_one` / `SimpleDispatcher::pump` /
`SimpleDispatcher::run_one` / `VirtualTimeExecutor::advance` /
`VirtualTimeExecutor::run_until_idle` /
`aria::abi::SlotErased::invoke`'s trampoline / async detached path)
funnels through
`aria::report_callback_failure(category, std::current_exception())`
— **bare `catch (...) { /* swallow */ }` is no longer used**.

**Contract**:
1. **Never throws**: `report_callback_failure` itself is
   `noexcept`; if the sink throws, the framework falls back to
   stderr. The "framework-internal `noexcept` boundary never calls
   `std::terminate`" promise is grounded here.
2. **One physical slot across SHARED modules**: storage for the
   sink and the abi slot hook lives in `libaria_abi` (single TU,
   strong definition), exported via `ARIA_ABI_API`; the exe and
   every SHARED library (`libaria_runtime.dylib` /
   `libaria_binding.dylib` / `libaria_qt6.dylib` / ...) share the
   same physical slot. The "one inline static per DSO" duplication
   problem cannot occur.
3. **Layering**: the abi layer cannot back-depend on `core`.
   `SlotInvokeFailureHook` (`<aria/abi/slot_factory.hpp>`) is the
   abi-provided injection point, bridged to
   `aria::report_callback_failure("abi.slot.invoke", …)` at startup
   by `aria::runtime::install_default_diagnostics()`. Layering and
   cross-dylib consistency both hold.
4. **Bootstrap idempotency**: a second
   `install_default_diagnostics()` returns `false` (already
   installed; no-op). `uninstall_default_diagnostics()` restores
   the sink/hook configuration that existed before the first
   install (typically `nullptr`, stderr fallback).
5. **Async dual fan-out**: the legacy `aria::async::set_error_sink`
   / `report_async_error` API is retained but routes through
   `report_callback_failure(category="async", message)` underneath.
   So: a host that installs only the core sink also observes async
   errors; a host that installs only the async sink keeps the
   historical behaviour; with both installed, both surfaces
   receive the event.
6. **Category naming**: dotted, `module.subsystem.action` form
   (`executor.thread_pool.worker` /
   `runtime.simple_dispatcher.pump` / `abi.slot.invoke` / `async`).
   New boundaries MUST follow this convention; otherwise
   prefix-based log routing on the host side breaks.

**Anti-patterns**:
- Adding a new internal `try { fn(); } catch (...) {}` without
  reporting — destroys observability, and CI greps for it (see
  `lints/`).
- Including a core header from the abi layer to call
  `report_callback_failure` directly — breaks layering; the build
  fails.

> **Future**: if a host wants to enrich the sink with source-line /
> thread-id / stack-capture, it can swap the sink without touching
> any boundary. That's the actual value of the unified channel —
> **all reporting paths converge behind one sink**.

### L-32: BindingEngine view-destroy path

When a view is destroyed: `IView::~IView` → `fire_destroy_()` →
`destroy_signal_.emit()`. The emit fires at the start of the
`IView` base destructor; handlers MUST NOT touch derived-class
state (the derived part has already destructed).

`BindingEngine` subscribes to `on_destroy` from `bucket_for_(view)`:
1. Clears the `per_view_[view]` bucket (releases every binding
   subscription tied to that view).
2. `view_alive_.erase(view_ptr)` drives the alive-sentinel reference
   count to zero, so `AliveToken::expired() == true`.
3. `per_view_.erase(view_ptr)`.

**Implication**: between `dispatcher.post(fn)` and the actual
execution of `fn`, the view may die. `fn` checks
`alive_token.expired()` and is silently dropped.

**Strongly recommended (not enforced)**: native adapters SHOULD
**proactively** call `fire_destroy_()` when the native handle is
released. The fallback fire from `IView::~IView` is too late — the
derived state is already gone.

### L-33: BindingEngine vs view destruction order

- Engine dies first: `engine_holders_` destruction → all
  `on_destroy` subscriptions release → even if the view dies later,
  our handler does NOT fire. `per_view_` destruction releases every
  bucket.
- View dies first: see L-32.
- Concurrent destruction across threads: **NOT allowed**. Aria UI
  subsystems are single-threaded; engine and view destruct on the
  same UI thread.

### L-32.5: Converter failure semantics (no more silent zero)

`aria::binding::Converter<T,U>`'s View → Model channel used to be
"`std::stoi/stod` throws → `catch(...)` → return `T{}`", which
silently wrote `0` / `0.0` into the ViewModel on bad input — the
business code couldn't tell "user typed 0" from "input is invalid".
Sprint4-#1 promoted this into an observable, non-corrupting
contract:

1. **New non-throwing channel `try_to_model`**: the field type is
   `std::function<std::optional<T>(const U&)>`; `std::nullopt`
   means "cannot parse". The built-in converters
   (`int_to_string` / `double_to_string` / `bool_to_yes_no` /
   `identity_string`) all populate this field.
2. **Strict `to_model`**: `std::stoi(s, &consumed)` /
   `std::stod(s, &consumed)` followed by an assert
   `consumed == s.size()` — `"12abc"` is now treated as bad input
   instead of `12`. On failure it **throws
   `aria::binding::ConversionError`** (a subclass of
   `std::runtime_error`); it no longer returns 0.
3. **Engine-side contract**:
   `BindingEngine::bind_text_converted` inside the adapter's
   `on_text_changed` callback:
   - Prefers `try_to_model`: if it returns `nullopt`, **completely
     skip `prop.set(...)`** and report through
     `aria::report_callback_failure("binding.converter", nullptr,
     "converter.try_to_model rejected input")`.
   - If `try_to_model` is empty, falls back to `to_model` wrapped in
     `try { … } catch (...) { aria::report_callback_failure(
     "binding.converter", std::current_exception()); }`. Whatever
     the user converter throws is swallowed but **recorded**.
4. **Invariants**:
   - **Model is never poisoned with a bogus default**: on bad
     input the ViewModel keeps its previous value; the UI keeps
     showing the user's invalid text until the adapter overwrites
     it (or VM logic triggers a fresh `to_view` push).
   - **Never silent**: every bad-input event leaves a
     `binding.converter` record in the host's sink. Test and
     production diagnostics speak the same language.
   - **Backward compatible**: legacy
     `c.to_view(x)` / `c.to_model(s)` callers continue to work;
     `to_model` simply upgrades from "return 0 on bad input" to
     "throw `ConversionError`", and that exception never escapes
     the engine.
5. **Physical evidence**: `grep -E "return 0;|return 0\.0;|catch
   \(\.\.\.\) \{ return"` over
   `modules/binding/include/aria/binding/converter.hpp` MUST yield
   zero hits — a hard CI check.

### L-34: ViewModel destruction order

`ViewModel::~ViewModel` order:
1. **Destroy hooks fire LIFO** (most recently added first); a
   throwing hook is reported via the callback-failure sink.
2. Default member destruction: `children_` → `bag_` →
   `on_destroy_hooks_` (already empty) → base members.

Implications:
- Place **active cancellation** cleanup (e.g.
  `ViewModelScope::cancel`) in `add_destroy_hook` — don't rely on
  default member destruction order.
- `bag_` (SubscriptionBag) destructs AFTER hooks — so `bag_` is
  alive when hooks run.
- `children_` destructs AFTER hooks — children are alive when
  hooks run.

### L-35: ViewModelScope cancellation timing

`ViewModelScope::attach(vm)` registers a hook via
`add_destroy_hook`; the hook calls `keep->cancel_and_join()` to
synchronously cancel and wait for every coroutine spawned by that
scope to exit (with a 5-second timeout). `on_cancel` still fires
synchronously on the VM destruction thread — its job is signal
propagation. The "wait for every coroutine to exit" part is
handled by `CoroutineScope`'s internal `inflight` counter +
condition variable.

Implications:
- The hook fires while the VM is still alive (per L-34), so
  accessing `*this` from the cancel callback is safe.
- A detached coroutine cancelled on a worker thread sees its
  wrapper run the final `inflight--`, wakes the condition variable,
  and unblocks the destruction thread's `cancel_and_join()`. By
  the time the VM destructor returns, every coroutine spawned by
  the scope has either exited or been recorded as a leak via the
  async error sink — none silently outlive the VM.
- The 5-second timeout only fires if the user fails to probe with
  `throw_if_cancelled` / `co_await tok` and uses a non-cancellable
  await. That is a user contract violation; the framework's safety
  net is "report and don't block process exit".

### L-36: CoroutineScope structured-concurrency contract

`CoroutineScope` is now a **real structured-concurrency primitive**,
not the lightweight "fire and forget" wrapper it used to be.
Contract:

1. **Accounting**: `launch(factory)` / `launch_simple(task)` wraps
   each coroutine; the wrapper does `inflight++` on entry,
   `inflight--` on exit, and notifies any waiter via the condition
   variable. `inflight_count()` is observable from any thread.
2. **Three wait paths**:
   - `cancel()`: non-blocking, just signals.
   - `cancel_and_join(timeout = 5s)`: synchronously waits for
     every wrapper to exit; on timeout, calls
     `report_async_error("CoroutineScope: dtor leaked N task(s)
     ...")`.
   - `co_await scope.join()` / `co_await scope.join_existing()`:
     cooperative wait from another coroutine, no timeout (the
     former cancels first; the latter does not).
3. **Error sink boundary**: inside the wrapper,
   `try { co_await body; } catch (OperationCancelled&) { /*
   expected, swallow */ } catch (std::exception& e) {
   report_async_error(...); } catch (...) {
   report_async_error(...); }`. `OperationCancelled` is the
   expected path and does not bother the sink; everything else
   goes to the sink rather than vanishing.
4. **Strong join in dtor**: `~CoroutineScope` automatically calls
   `cancel_and_join(5s)` — the C++-side embodiment of structured
   concurrency's "no coroutine outlives its scope" invariant. Even
   if the user forgets to call join, no detached coroutine flies
   past the scope.
5. **Parent-child cascade**: `CoroutineScope child{parent.token()};`
   — when the parent cancels, a callback holding a weak reference
   to the child's state cancels the child. If the child dies
   first, the callback safely no-ops (an internal
   `CancellationSourceProxy` + `ProxyDetacher` ensures the child's
   source is either alive or explicitly detached when the parent's
   cancel arrives).

`CancellationSource::cancel()`'s own semantics are unchanged:
1. CAS `cancelled_` to true; on a race, only one winner runs the
   callbacks.
2. Move out the entire `callbacks_` list and fire serially outside
   the lock.
3. `~CancellationSource` cancels automatically.

> **`on_cancel` fires immediately if cancellation already
> occurred**: the implementation does a lock-free
> `is_cancelled()` first and fires synchronously when already
> cancelled; even if cancellation lands while holding the lock,
> the rechecked-and-fire path catches it. See
> `CancellationToken::on_cancel` in `cancellation.hpp`. This is
> directly load-bearing for the parent-child cascade — if a parent
> cancels before the child registers, the child fires its own
> cancel right away rather than missing it.

### L-37: AsyncCommand cancellation propagation

Each `execute()` creates a per-invocation `CancellationSource`
that registers into the state's `invocation_sources` list.
- AsyncCommand destruction → `state->cancel.cancel()` → the
  coroutine token of every invocation flips.
- Under the `LatestOnly` policy, a new `execute` triggers
  `cancel_all_in_flight()` to cancel every pending invocation.
- The coroutine probes `throw_if_cancelled` at two safe points
  (after `schedule_on(ui)` and after `schedule_on(worker)`).

Implications:
- After `AsyncCommand` is destroyed, in-flight coroutines may run
  for a while longer — they hold a `shared_ptr` to `state`, so
  `state` is not freed when `AsyncCommand` is.
- Property writes from a post-destruction coroutine **still
  happen** — but the Property is a member of `state`, so no UAF.
  However: a typical setup is "ViewModel owns AsyncCommand"; when
  the VM destructs, the Properties (`is_executing` /
  `last_error` / `last_result`) on `state` are still alive; the
  final `co_await schedule_on(ui)` still tries to post to the
  original ui executor, and a dead ui executor causes the post to
  fail or leak (executor-implementation defined).
  > **Strong contract**: **AsyncCommand MUST die before the ui
  > executor it depends on**. The usual pattern is "ui executor
  > as an app singleton, AsyncCommand as a VM member".

### L-38: AsyncResource lifecycle

`AsyncResource<T, Key>` maintains cache + dedupe and shares the same
dual-executor model as `AsyncCommand` (L-37). Its public observable
surface is four equality-gated Properties (`is_loading` / `error` /
`error_message` / `data`) plus a synthesised `Property<Loadable<T>>`
that always agrees with them (LO-1).

**Generation / staleness (R-1)**: every `fetch()` / `refresh()` bumps
an atomic `gen`. A run reads its `my_gen` at launch and, after the
final `schedule_on(ui)` hop, compares it against the live `gen`. Only
the latest run wins; a stale run drops its result and **MUST NOT clear
`in_flight`** — the newer run owns the flag and clears it on its own
completion. This keeps `is_loading` continuously true across rapid key
changes instead of flickering.

**Cancellation axes**: `AsyncResource` participates in all three
lifetime axes (see "Three-axis cancellation" below):
- **Owner destruction**: `~AsyncResource` calls
  `state_->cancel.cancel()`. The `state_` is a `shared_ptr`, so any
  in-flight coroutine keeps it alive and unwinds cleanly at its next
  `throw_if_cancelled` probe; Property writes from a late run land on
  the still-alive `state_` (no UAF), and the equality gate makes them
  harmless.
- **View destruction**: `cancel()` (public) flips the in-flight tokens,
  re-arms a fresh `CancellationSource` (a cancelled source stays
  cancelled forever), bumps `gen` so any late run is dropped, and
  clears `is_loading` while **preserving `data`** (SWR — a re-mounted
  view still renders the last value). The resource stays fully usable.
  Wire it via `BindingEngine::bind_view_lifetime(view, [&]{
  resource.cancel(); })`.
- **Scope cancellation**: if the resource's `ui`/`worker` executors are
  owned by a `ViewModelScope`, scope teardown (L-35) drains them.

> **Strong contract (same as L-37 A5)**: `AsyncResource` MUST die before
> the `ui` executor it depends on. The usual pattern is "ui executor as
> an app singleton, resource as a VM member".

### L-38.1: Three-axis cancellation model

In-flight async work is cancelled along exactly three independent
lifetime axes. Each has a distinct owner and trigger; they compose
without overlap.

| Axis | Owner | Trigger | Mechanism |
|---|---|---|---|
| **VM scope** | `ViewModelScope` | VM destroy hook (L-34/L-35) | `CoroutineScope::cancel_and_join()` |
| **Navigator entry** | `Navigator` entry | entry pop | the entry's `CancellationSource` |
| **View destroy** | the `IView` | native handle released → `IView::on_destroy` | `BindingEngine::bind_view_lifetime(view, cb)` runs `cb` once |

The view-destroy axis is the one a naive MVVM framework misses: a user
navigates away from a *sub-view inside a still-living page* (so neither
the VM nor the Navigator entry is torn down), yet an in-flight
`AsyncCommand` / `AsyncResource` is still running and would resume
against a dead view. `bind_view_lifetime` closes it:

```cpp
AsyncCommand<void> load{ui, [](CancellationToken t) -> Task<void> {
    co_await fetch(t);                       // cooperative cancel point
}};
engine.bind_command(load.trigger(), view);  // click → execute
engine.bind_view_lifetime(view, [&load]{
    load.cancel_all_in_flight();             // view gone → cancel request
});

// AsyncResource uses the same hook:
engine.bind_view_lifetime(view, [&resource]{ resource.cancel(); });
```

**Layering note**: `BindingEngine` itself is deliberately async-agnostic —
`bind_view_lifetime` takes a plain `std::function<void()>` and the engine
never reaches into `AsyncCommand`. (The `binding` module *does* link
`aria-async`, because sibling facilities like `ViewModelScope` and
`Navigation` need coroutine cancellation primitives; but the binding engine
and its `bind_*` methods stay free of any async type.) So the host wires the
async side in one explicit line rather than the engine reaching into
`AsyncCommand`. There is no async-aware `bind_*` overload on the engine; if
one were ever wanted it would belong to a layer above (the app, or a future
`aria-app` umbrella), not to `BindingEngine`. See ROADMAP P1-H.

### L-39: Adapter platform_name and unsupported-widget diagnostics

`IViewAdapter` implementations MUST honour two invariants:

1. **`platform_name()` is a stable lowercase id** that exactly
   matches `IView::kind()` (Qt6→`"qt6"` / AppKit→`"appkit"` /
   UIKit→`"uikit"` / Fake→`"fake"`). Trace events and log filters
   route on this string — case drift breaks "filter diagnostics by
   platform" regexes. New adapters MUST follow the lowercase id
   rule.
2. **Unsupported widgets MUST warn**: `set_*` / `get_*` /
   `on_*_changed` / `on_click` / `set_visible` / `set_enabled`,
   when handed a widget class outside the adapter's support
   matrix, MUST first call the adapter-internal
   `warn_unsupported_(op, native)` helper (writing through
   `aria::runtime::Logger::warn` under the `qt_adapter` /
   `appkit_adapter` / `uikit_adapter` category), then return
   safely with a zero subscription / default value / no-op.

**Why warn is NOT rate-limited**: the unsupported branch only
fires when "the user bound the wrong widget" — that's a real
diagnostic signal. Throttling it would erase the difference
between the first and the ten-thousandth occurrence and wipe out
its diagnostic value. Adapters are hot paths only on the
"matched widget" branch — that path bypasses the warn helper.

**Why all three adapters must be symmetric**:
- A cross-platform ViewModel test can verify Qt6 / AppKit / UIKit
  with one body of host code; the warn-string format
  (`<op>: no binding path for widget|view class '<cls>'`) must
  match across platforms.
- The legacy AppKit / UIKit pattern of
  `if (![o isKindOfClass:...]) return {};` (silent early return)
  is exactly the kind of "dark hole" a top-tier framework must
  not have — a developer who mis-binds a widget sees no UI
  response and a clean log, making diagnosis very expensive. The
  new contract pins this diagnostic path so that "mis-bind" shows
  up in the logs immediately.

**Type recognition**:
- Qt6 side uses `o->metaObject()->className()` to obtain the
  widget class name.
- AppKit / UIKit side uses `<objc/runtime.h>`'s
  `object_getClassName(o)` — no ARC dependency, no cost.

---

### L-40: Container teardown is reverse registration order

`runtime::Container` releases its registrations in **reverse
registration order**, in both `clear()` and `~Container()`. A service
registered *after* its dependency is therefore destroyed *before* it, so
the rule for hosts is one sentence: **register providers before
consumers.**

Two further guarantees hold during teardown:

1. **Each value is destroyed with the container mutex released.** A
   service destructor MAY call back into the container (`resolve` /
   `has` / `register_*`) without self-deadlocking. This is the same
   hazard as A11 — an owner destroying its values while holding its own
   lock, where the value's destructor re-enters that owner.
2. **Entries not yet reached are still resolvable.** While entry *N* is
   being destroyed, entries *1..N-1* remain registered, so a consumer's
   destructor can still reach a provider registered before it.

Singleton registrations and factory registrations share **one** teardown
order; they are not two independently cleared tables. Re-registering a
type replaces the value and destroys the previous one immediately (also
outside the lock) but **keeps the type's original position** — the
instance behind the type changed, the dependency order did not.

**Scope limit — this is registration order, not the resolution graph.**
Aria does not record who resolved whom during construction, so a
consumer registered *before* the provider it resolves still tears down in
the wrong order. That is a caller-side bug the container cannot detect.
Full dependency-graph-ordered teardown is deliberately out of scope (see
`docs/ROADMAP.md` → *Evaluated and declined*).

`Container` has no `global()` accessor — every instance is explicitly
owned by its caller — so this contract is per-instance and introduces no
process-wide state.

Pinned by `test_container.cpp`; reverting teardown to forward order (or
to `unordered_map::clear()`) fails five of its cases, including the
re-entrant-destructor case, which deadlocks rather than merely
mis-ordering.

---

## 4. Anti-patterns

| # | Anti-pattern | Consequence | Correct approach |
|---|---|---|---|
| A1 | `prop.set(x)` from a worker thread | Debug trips `assert_on_graph_thread`; Release is undefined | First `co_await schedule_on(ui)` |
| A2 | Reactive `Subscription` destructed off the graph thread | Node detach happens on the wrong thread, can race graph internals | Destruct on the graph thread (e.g. destroy the VM there) |
| A3 | Multi-node cycle: Effects/Computeds writing each other's read source (A→B→C→A) | After 100 rounds: `CircularDependencyError` | Redesign the dependency direction; use `peek` to read prior frame |
| A3b | An Effect body does `prop.set(prop.get()+condition)` writing its own read source | Does NOT trigger a cycle (the clear_sources pattern dodges it), but every external write makes `fn` run twice — once from the external write, once from the Effect's own set in the next round. Can violate user expectations | Use `peek()`, or an explicit idempotent guard, or redesign |
| A4 | Touching derived-class members in an `IView::~IView` handler | Derived has already destructed → UAF | Call `fire_destroy_()` early in the native destructor |
| A5 | The ui executor dies right after AsyncCommand | The write-back post fails or leaks | AsyncCommand MUST die before the ui executor (see L-37) |
| A6 | `prop.set` inside a `Computed` body that loops the dependency graph | Same as A3 | Wrap in batch or redesign |
| A7 | `Direct` BindingEngine + multi-thread emit | A worker thread may touch a native widget — UB | Use `SmartMarshal` or guarantee emit on the UI thread |
| A8 | `ObservableList` slots holding cycles (slot.item points back at itself or each other) | Never released, leak | Avoid on the business side; the framework does not police it |
| A9 | The same `Subscription` moved-from multiple times / destructed across threads | move-only forbids copy, but cross-thread move can still UAF (reactive backend) | Hold on the graph thread; if needed, post a reset task through the dispatcher |
| A10 | Destroying a Reaction node currently being scheduled inside a reactive flush | The round vector holds raw pointers — potential UAF | Don't `bag.clear()` your own Effect from inside that Effect's body |
| A11 | An adapter destroying its cached `IView` wrappers while holding its own mutex | `~IView` fires `on_destroy`, whose handlers include the adapter's own bridge cleanup — which re-locks that mutex and self-deadlocks | Move the cache out under the lock, then destroy it after the lock is released (see `QtAdapter` / `AppKitAdapter` / `UIKitAdapter` teardown, and `test_appkit_view_for.mm`) |

---

## 5. Mapping to the P0-ε reliability stress suite

P0-ε must add a fuzzer for each of the contracts below:

| Invariant | Fuzzer | Goal |
|---|---|---|
| L-13 unsubscribe-during-emit | `signal_unsubscribe_during_emit_fuzzer` | 1M emits × random disconnects, no UAF / no missed disconnect |
| L-17 dynamic dependencies | `computed_dynamic_dep_fuzzer` | 1M random branch flips, no ghost subscriptions |
| L-20 set during reactive flush | `reactive_reentrant_set_fuzzer` | Recursive set depth ≤ 50, eventually stable or `CircularDependencyError` |
| L-32 binding view-destroy race | `binding_view_destroy_race_fuzzer` | 1M emit-vs-destroy interleavings, no UAF |
| L-31 list mutation storm | `observable_list_mutation_storm_fuzzer` | 1M random insert/remove/move/replace, listeners stay consistent |
| L-36 structured concurrency | `coroutine_scope_drain_fuzzer` | 1M launch/cancel/join interleavings; no missed callback, no missed accounting, `inflight_count` converges to 0 |
| L-37 async command cancel/dtor race | `async_command_dtor_fuzzer` | 1M dtor-vs-execute interleavings, no UAF / no leaked coroutine |

Any failure here = **contract break**. Fix the code, not the test.

---

## 6. Document governance

This document is part of the P0-β deliverable. Any future
lifecycle-related change MUST flow as **doc change → code change →
test change**; the reverse is forbidden (to prevent "code drifts
first, doc catches up later" contract decay).
