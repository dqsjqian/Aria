# Aria API Style Contract

> This document is the framework's authoritative reference for **API
> style**. Together with [`lifecycle.md`](./lifecycle.md) it forms
> the standard "read these and you can write Aria-style code".
> Any naming, namespacing, include-path, error-message, template-
> diagnostic, or deprecation argument should ultimately cite this file.
> Every contract item is numbered `S-N` for citation in code and
> commit messages.
>
> A "best-in-class C++ MVVM framework" API must satisfy three
> overarching principles:
> 1. **Consistent**: equivalent things appear in the same shape across
>    every module.
> 2. **Direct**: a user knows what to autocomplete BEFORE typing the
>    first character.
> 3. **Diagnosable**: misuse produces a one-line, human-readable
>    error from the compiler — never a 30-frame SFINAE eruption.

---

## 0. Overview

```
Public namespace      Scope                          Users should use     Implementation namespace
-------------------------------------------------------------------------------------------------
aria::                Top-level public API (core)    yes — preferred      —
aria::async::         Async + coroutine public API   yes — preferred      —
aria::binding::       View binding + ViewModels      yes — preferred      —
aria::runtime::       App runtime services           yes — preferred      —
aria::abi::           ABI boundary type-erasure      ⚠ advanced users    —
aria::reactive::      Reactive implementation        no — internal        yes
aria::detail::        Type-erasure signal wrappers   no — internal        yes
aria::*::detail::     Per-module impl details        no — internal        yes
aria::*::testing::    Framework-bundled test kits    yes — for tests      —
```

---

## 1. Namespace contract

### S-1: single public entry point

**Core MVVM types are exposed under `aria::`** — users facing the
core reactive API write only `aria::`:

| Category | Public symbol | Notes |
|---|---|---|
| Reactive primitives | `Property<T>` / `Computed<T>` / `Effect` | State, derived values, side effects |
| Subscription | `Subscription` / `SubscriptionBag` | Unified RAII detach |
| Command | `Command<Args...>` | Synchronous command |
| Collections | `ObservableList<T>` / `FilteredList<T>` / `SortedList<T>` / `MappedList<U,V>` / `ListChange<T>` / `ListChangeKind` | Collections + derived collections |
| Validation | `Validator<T>` / `ValidationState` | Per-field validator + form state |
| Abstractions | `IProperty<T>` | ABI-friendly interface |
| Globals | `batch` / `untracked` / `BatchScope` / `UntrackedScope` / `dep` | Control primitives |
| Exceptions | `CircularDependencyError` | Reactive failure signal |
| Concepts | `PropertyValue` / `EqualityComparable` / `Observable` / `InvocableR` | Template constraints |

**Per-domain public exports**:

- `aria::` (root namespace, unified scheduler base) — `IScheduler` /
  `SchedulerCaps` / `has_caps` / `require_caps` /
  `unsupported_capability` / `IDelayedScheduler`.
- `aria::` (hot-path zero-allocation callable utilities) —
  `function_ref<R(Args...)>` /
  `inplace_function<R(Args...), N, Align>` /
  `bad_inplace_function_call`.
- `aria::` (unified callback-failure reporting channel) —
  `CallbackFailure` / `CallbackFailureSink` /
  `set_callback_failure_sink` / `current_callback_failure_sink` /
  `report_callback_failure`.
- `aria::async::` — `Task` / `AsyncCommand` / `AsyncResource` /
  `Channel` / `CancellationSource` / `CancellationToken` /
  `OperationCancelled` / `with_timeout` / `when_any` /
  `when_any_cancellable` / `when_all` / `IExecutor` /
  `InlineExecutor` / `VirtualTimeExecutor`.
- `aria::binding::` — `BindingEngine` / `IView` / `IViewAdapter` /
  `Converter<T,U>` / `ConversionError` / `ViewModel` /
  `ViewModelScope`; adapter conformance facilities live under
  `aria::binding::testing::conformance::`.
- `aria::runtime::` — `Logger` / `Container` / `IDispatcher` /
  `EventBus` / `install_default_diagnostics` /
  `uninstall_default_diagnostics`.
- `aria::abi::` — the v-table bridge for `IProperty`, `SignalErased`
  / `SlotErased`, and `SlotInvokeFailureHook` /
  `set_slot_invoke_failure_hook` (only relevant when writing a
  cross-dylib bridge).

> **Unified scheduler base**: every scheduler (`IExecutor` /
> `IDelayedScheduler` / `IDispatcher` and their subclasses) virtually
> inherits from `aria::IScheduler` and reports its capabilities via
> `caps()` returning a `SchedulerCaps` bitmask. A component checks
> requirements with a single
> `has_caps(s, SchedulerCaps::Delay | SchedulerCaps::MainThread)` line;
> if the capability is missing, `require_caps` throws
> `unsupported_capability`. The legacy `IExecutor::post` /
> `IDelayedScheduler::post_after` / `IDispatcher::post_delayed` are
> retained as equivalent aliases of `IScheduler::schedule` /
> `schedule_after`.

> **Hot-path callable contract**:
> - `aria::function_ref<R(Args...)>` — non-owning,
>   `sizeof == 2 * sizeof(void*)`, trivially copyable. Use it as a
>   parameter type to accept any callable without forcing a
>   `std::function` copy / heap allocation. **Never** store it as a
>   field — it does not extend the target's lifetime.
> - `aria::inplace_function<R(Args...), N=32, Align=alignof(max_align_t)>`
>   — owning, `N`-byte inline buffer; **capacity overflow is a
>   compile-time `static_assert`, never a heap allocation**. Copyable /
>   movable iff the erased callable is copyable / movable. Use cases:
>   (1) derived-list owning callbacks (`FilteredList::Predicate` /
>   `SortedList::Comparator` / `MappedList::Mapper` /
>   `DistinctList::KeyOf` / `GroupedList::KeyOf` are switched to
>   `inplace_function<…, 32>`; the zero-heap-allocation contract is
>   type-system enforced); (2) anywhere you want to keep a lambda
>   long-term but absolutely forbid it from silently calling `malloc`.
> - Selection rule: **short-lived sync callback** → `function_ref`;
>   **long-lived storage with known capacity** → `inplace_function`;
>   **long-lived storage with unknown capacity / crossing an ABI
>   boundary** → `std::function`. Together they form the
>   "non-owning, fixed-capacity owning, unbounded owning" trio.

> **Unified callback-failure reporting channel**: every framework-
> internal boundary that "must stay `noexcept` yet calls into user
> code" (thread-pool worker / main-thread drain & run_one /
> SimpleDispatcher pump & run_one / VirtualTimeExecutor advance &
> run_until_idle / ABI slot trampoline / async detached path) MUST
> route through
> `aria::report_callback_failure(category, std::current_exception())`.
>
> - **Category naming**: dotted, `module.subsystem.action`, e.g.
>   `"executor.thread_pool.worker"` / `"executor.main_thread.drain"` /
>   `"executor.main_thread.run_one"` /
>   `"runtime.simple_dispatcher.pump"` /
>   `"runtime.simple_dispatcher.run_one"` /
>   `"executor.virtual_time.advance"` /
>   `"executor.virtual_time.run_until_idle"` /
>   `"abi.slot.invoke"` / `"async"` (legacy async surface).
> - **Storage location**: `sink_storage()`'s real definition lives in
>   `libaria_abi` (single TU); **every SHARED module shares the same
>   physical slot**, avoiding the "inline static across DSOs"
>   duplicate-storage problem. The slot-failure hook
>   (`aria::abi::set_slot_invoke_failure_hook`) follows the same
>   model.
> - **Default behaviour**: with no sink installed, stderr emits one
>   line `[aria.callback_failure] <category>: <message>`. The host
>   application calls `aria::runtime::install_default_diagnostics()`
>   in `main()` to bridge every sink to `aria::Logger::error`, with
>   the category prefixed by `aria.` (e.g.
>   `aria.executor.thread_pool.worker`).
> - **Invariant**: `report_callback_failure` itself never throws —
>   if a user-installed sink throws, the framework's stderr fallback
>   handles it. This guarantees no framework-internal `noexcept`
>   boundary ever calls `std::terminate`.
> - **ABI bridge**: the abi layer cannot back-depend on core; the
>   `SlotInvokeFailureHook` is abi's injection point, bridged to
>   `report_callback_failure("abi.slot.invoke", …)` at startup by
>   `runtime::install_default_diagnostics()`.

> **Converter failure-semantics contract**: historically
> `aria::binding::Converter<T,U>` had only `to_view` / `to_model`
> fields, and the built-in `to_model` silently returned `T{}` on a
> parse failure (`int` → 0, `double` → 0.0). The business code could
> not distinguish "user typed 0" from "input is invalid".
>
> - **New field**: `std::function<std::optional<T>(const U&)> try_to_model`.
>   `std::nullopt` means "cannot parse"; this is the channel the
>   binding engine prefers.
> - **Strict to_model**: built-in converters (`int_to_string` /
>   `double_to_string` / `bool_to_yes_no`) **throw
>   `aria::binding::ConversionError`** (derives from
>   `std::runtime_error`) on parse failure rather than silently
>   returning 0. The `try_to_model` field is filled with the
>   equivalent non-throwing implementation.
> - **Engine-side contract**: `BindingEngine::bind_text_converted`
>   on the View → Model path:
>   1. Calls `try_to_model` first; on `std::nullopt` **does not
>      write the Model** and reports via
>      `aria::report_callback_failure("binding.converter", nullptr,
>      "converter.try_to_model rejected input")`.
>   2. When the user-supplied converter does not populate
>      `try_to_model`, falls back to `to_model` wrapped in a
>      `try/catch` that routes to the same channel (category
>      `"binding.converter"`). Both paths guarantee the Model is
>      **never written with a fabricated default value**.
> - **Trailing-garbage strictness**: the built-in numeric converters
>   use `std::stoi(s, &consumed)` / `std::stod(s, &consumed)` and
>   assert `consumed == s.size()`, so `"12abc"` is detected as
>   invalid input rather than `12`.
> - **Backward compatibility**: the legacy entry points
>   `c.to_view(x)` / `c.to_model(s)` are retained — `to_model` simply
>   upgrades from "return 0 on bad input" to "throw, caught by the
>   engine". Hosts will see `binding.converter` events in their
>   logs; the model is no longer silently corrupted.

> **BindingEngine trace helper contract**:
> `BindingEngine::dispatch_to_view_<Fn>` is a template member, so it
> instantiates once per binding parameter `Fn`. Three branches
> (`Direct` / `SmartMarshal` on the main thread / `dispatcher.post`)
> each need to emit a `TraceCategory::Binding` event at two points
> ("view destroyed" and "VM→View write") — six
> `publish_trace_unchecked` call sites in total.
>
> - **Helper abstraction**: two non-template static members,
>   `BindingEngine::trace_drop_(std::string_view platform) noexcept`
>   and `trace_emit_(std::string_view platform) noexcept`, are the
>   only entities behind those six call sites. They encapsulate the
>   `aria::trace::Binding{ platform_str, "", "view_destroyed_drop" /
>   "vm_to_view" }` payload construction and the
>   `publish_trace_unchecked` publish.
> - **Guards retained**: call sites still have
>   `if (tracing) trace_drop_(platform)` /
>   `if (::aria::has_trace_sink()) trace_emit_(platform_copy)` —
>   ensures we pay zero call cost when tracing is off (a `noexcept`
>   helper still has to copy the input string into its parameter,
>   which is what the guard short-circuits). The template body is
>   not slowed by the helper extraction.
> - **Why non-template**: the helpers are intentionally regular
>   functions, not helper templates, so that
>   `dispatch_to_view_<Fn>`'s template bloat does not also copy the
>   trace payload-construction code into every instantiation.
>   BindingEngine has 5 instantiation branches today — they share
>   the same helper code, zero duplication.
> - **Why static**: helpers don't depend on `*this`, only on the
>   platform string. Making them `static` lets dispatcher-posted
>   lambdas call `BindingEngine::trace_drop_(platform_copy)`
>   directly without capturing `this`, structurally avoiding the
>   "engine destructed but in-flight lambda still references this"
>   dangling-access risk.
> - **Future trace events**: any new BindingEngine trace event
>   added later MUST follow the same four invariants: non-template +
>   `static` + `std::string_view` parameter + `noexcept`, with a
>   `has_trace_sink()` / `tracing` guard at the call site.

### S-2: `aria::reactive::` is the implementation namespace

**Forbidden**:
- Public documentation examples that use the long-form
  `aria::reactive::Property`.
- User tutorials and READMEs demonstrating `aria::reactive::*`.

**Allowed**:
- Mutual references between internal implementation headers.
- The very rare cases where a user genuinely needs low-level APIs
  like `Graph::set_graph_thread()` / `Graph::is_on_graph_thread()`.
- `aria::reactive::GraphInspector` is **a public diagnostic tool**,
  but should be accessed via the promoted alias `aria::GraphInspector`
  (see S-3).

### S-3: diagnostic tools are also promoted into `aria::`

`reactive::GraphInspector` MUST be promoted via
`using reactive::GraphInspector` to `aria::GraphInspector`.
Anything users **should** use must not require typing the
implementation namespace. This is a concrete instance of S-1.

### S-4: `aria::detail::` is the type-erasure bridge above ABI

`aria::detail::TypedSignal<...>` and `aria::detail::ReactionNode` etc.
wrap `aria::abi::SignalErased` and `Node`-like ABI primitives into
strongly-typed internal bridges.

**Users MUST NOT use them directly.**
Inside the framework they MUST be referenced fully qualified as
`aria::detail::TypedSignal<...>`; bare `detail::TypedSignal` is
**forbidden** because if a user wrote `using namespace aria::reactive`,
the bare `detail::` would resolve to `aria::reactive::detail` and
become ambiguous.

### S-5: per-module `*::detail::` follows the single-file principle

`aria::async::detail::` / `aria::reactive::detail::` / each module's
`detail::` namespace:
- **Only accessible from within the same module's implementation
  files.**
- Must not appear in that module's public-header signatures (showing
  up in private/protected/static helpers is fine).
- "Implementation details" that need cross-module sharing must be
  promoted to `aria::detail::` or `aria::abi::`, not borrowed across
  modules from someone else's `detail::`.

---

## 2. Include-path contract

### S-10: user-facing includes always use `<aria/...>`

```cpp
#include <aria/property.hpp>
#include <aria/observable_list.hpp>
#include <aria/async/async_command.hpp>
#include <aria/binding/binding_engine.hpp>
```

Never require users to write deep paths like
`<aria/reactive/property.hpp>`. Deep paths are an implementation
detail.

### S-11: framework-internal includes also use the long `aria/` path

The framework's own .hpp / .inl files include each other via
**uniformly long `"aria/..."` paths**; relative paths and bare file
names are forbidden:

Correct:
```cpp
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/typed_signal.hpp"
```

Wrong:
```cpp
#include "subscription.hpp"           // relies on include path
#include "../subscription.hpp"        // relative path
#include "concepts.hpp"               // bare same-dir name
```

Reasons:
- IDE jump-to-definition is more stable.
- No reliance on the `PUBLIC` / `PRIVATE` ordering of CMake
  `target_include_directories`.
- grep / file-move-safe.
- Top-tier frameworks all follow this convention (Boost, Folly,
  abseil, Eigen, QtCore's own module includes).

### S-12: single facade umbrella header per module

Each module exposes one umbrella header as a "one-stop" entry; samples
and quickstarts are encouraged to use it:

| Umbrella | Module |
|---|---|
| `<aria/aria.hpp>` | All public core APIs |
| `<aria/async/async.hpp>` | All public async APIs (when present) |
| `<aria/binding/binding.hpp>` | All public binding APIs (when present) |
| `<aria/runtime/runtime.hpp>` | All public runtime APIs (when present) |

Sub-headers can still be included individually; the umbrella is just
a convenience entry point.

---

## 3. Naming-style contract

### S-20: types are `PascalCase`

`Property` / `ObservableList` / `BindingEngine` / `AsyncCommand`.
Acronyms are leading-cap-only, not all-caps: `IoExecutor`, not
`IOExecutor` (per Google Style).

### S-21: functions and methods are `snake_case`

`prop.set(v)` / `prop.on_changed(fn)` / `list.push_back(x)` /
`engine.bind_text(...)`.

### S-22: template parameters and concepts are `PascalCase`

```cpp
template<PropertyValue T>
class Property;

template<typename Fn>
    requires std::invocable<Fn>
void launch(Fn&&);
```

### S-23: members use `trailing_underscore_`

Private data members are `name_`:
```cpp
private:
    std::vector<Slot> slots_;
    std::shared_mutex mutex_;
```

### S-24: macros only when truly necessary, ALL_CAPS prefixed `ARIA_`

`ARIA_BINDING_API`, `ARIA_NO_DISCARD`, `ARIA_DEPRECATED`.
**Never** use a macro to declare a user-visible API; only for
platform branching and export decoration.

### S-25: async-entry naming

- Coroutine factory: `launch` / `start_detached`
- Awaitable factory: `schedule_on(executor)` /
  `schedule_after(scheduler, delay)`
- Combinators: `with_timeout` / `when_any` / `when_any_cancellable` /
  `when_all` / `retry`
- Cancellation: `CancellationSource` / `CancellationToken` /
  `throw_if_cancelled`

### S-26: observer-registration vs immediate-fire naming

| Shape | Naming | First-fire behaviour |
|---|---|---|
| Subscribe only, never fire | `on_changed(fn)` / `observe(fn)` / `on_destroy(fn)` / `on_click(fn)` | Not invoked |
| Fire once, then keep observing | `bind(fn)` | Synchronously invoked once |
| RAII side effect | `Effect e{fn}` | Construction fires once |

This naming contract is pinned in [`lifecycle.md`](./lifecycle.md)
L-19; this document just restates it.

---

## 4. Errors and diagnostics (see also P0-α.2 error model)

### S-30: compile-time diagnostic priority

Ordered by "how readable to the user", template entry points MUST
satisfy at least the first two:

1. **`concept` constraints**: appearing in the template parameter
   list — IDEs immediately surface "constraint not satisfied".
2. **`static_assert` fallback**: inside the template body, with a
   **one-line** explanation of the issue and a suggestion.
3. **SFINAE / `requires` clauses**: only as internal implementation
   detail, never as the first user-facing diagnostic line.

Anti-pattern (30-frame SFINAE):
```cpp
template<typename Fn>
auto AsyncCommand::Builder::action(Fn fn) {
    return /* ... wait until std::invoke fails and produces a wall of text ... */;
}
```

Correct:
```cpp
template<typename Fn>
    requires AsyncActionFn<Fn, T>     // S-30 line 1
auto Builder::action(Fn fn) {
    static_assert(!std::is_pointer_v<Fn>,
        "AsyncCommand::action expects an invocable; "
        "did you forget () after a function name?");   // S-30 line 2
    /* ... */
}
```

### S-31: runtime error messages must be locatable

A thrown exception's `what()` MUST contain:
- **What**: what happened ("reactive cycle detected").
- **Where**: location clue (node debug name / file / function).
- **How**: a hint about the next step ("check Effect that writes
  its own dependency").

`CircularDependencyError` already complies (carries the node-name
list). `OperationCancelled` is regular control flow and does not
need a "where".

### S-32: exceptions inside observers MUST be swallowed

Per [`lifecycle.md`](./lifecycle.md) L-13: exceptions thrown from a
user callback during a signal emit MUST be swallowed by the framework.
A misbehaving handler **MUST NOT** prevent later handlers from
running.

The reactive subsystem's `recompute()` is the exception: an
exception is propagated back to `Graph::pull`, which restores the
node to `Clean` and rethrows.

### S-33: adapter `platform_name` and unsupported-widget contract

`IViewAdapter::platform_name()` returns a **stable lowercase id**
that matches `IView::kind()` exactly:

| Adapter | `platform_name()` | `IView::kind()` |
|---------|-------------------|------------------|
| Qt6     | `"qt6"`           | `"qt6"`          |
| AppKit  | `"appkit"`        | `"appkit"`       |
| UIKit   | `"uikit"`         | `"uikit"`        |
| Fake    | `"fake"`          | `"fake"`         |

Reason: trace events / diagnostic sinks / log filters / showcase
routing all match on the `platform_name()` string. Mixing case
breaks "filter logs by platform" regexes. New adapters MUST follow
the lowercase id rule.

**Unsupported-widget behaviour**: every `set_*` / `get_*` /
`on_*_changed` / `on_click` that receives a widget class outside
the adapter's support matrix MUST go through the corresponding
`warn_unsupported_(op, native)` helper and emit one warning line:

```
<op>: no binding path for widget class '<cls>'      # Qt6 side
<op>: no binding path for view class '<cls>'        # AppKit / UIKit side
```

Routed via `aria::runtime::Logger::warn(category, msg)` with the
category set to `"qt_adapter"` / `"appkit_adapter"` /
`"uikit_adapter"`. After the warn, the adapter MUST safely return
with a "zero subscription `Subscription{}`" / "default value 0 / false
/ empty string"; throwing or accessing `nullptr` is forbidden.

Reasons:
- Old design on AppKit / UIKit was `if (![o isKindOfClass:...]) return {};`
  — silent early return — leaving ViewModels bound to the wrong
  widget with no clue. That is exactly the kind of "dark hole" a
  top-tier framework must not have.
- The three adapters' unsupported paths must be **symmetric**: same
  op + same widget mismatch → same warn format → same safe return
  value. A host's tests cover all three platforms with one body of
  code.
- The warn is not rate-limited. Adapters are hot paths but the
  unsupported branch only fires when "the user bound the wrong
  widget" — a real diagnostic signal that should not be throttled
  away.

---

## 5. Deprecation and compatibility

### S-40: the project ships zero `deprecated` APIs

Aria's external version stays at 1.0.0 forever; **during evolution
we keep no deprecated aliases**. Every P0/P1 closure breaks all call
sites outright in the same commit, and migrates the in-tree examples
/ adapters / tests in that commit too.

Reasons:
- A deprecated alias is "tomorrow's debt".
- The project goal is "best in class" — tolerating a deprecated
  block tolerates an inelegant block of code in the tree.
- During the no-external-user evolution window, this is the cleanest
  approach.

### S-41: source MUST NOT contain version literals

Per [[memory:j61ttodt]]:
- Source code / headers / inline docs / commit messages MUST NOT
  contain version literals like `v2.x`, `since 1.x`, `v1.0.0`.
- Version evolution lives only in `CHANGELOG.md` and `README.md`.
- Comments, documentation, and tests MUST be in English. Chinese is
  permitted only inside `examples/` (per-demo source) and the
  Chinese localised README files (`README.zh-CN.md` /
  `README.zh-CN.html`).

---

## 6. Style checklist (PR review checklist)

Before merging any PR, self-review:

```
[ ] Public APIs do not require the user to write aria::reactive::* / aria::detail::*
[ ] Includes use the long "aria/..." path (incl. .inl)
[ ] Template entries have BOTH a concept constraint AND a one-line static_assert
[ ] Naming follows PascalCase types / snake_case functions / trailing_underscore_ members
[ ] No new deprecated aliases
[ ] No version literals in source; no Chinese comments / docs / tests outside examples/ and README.zh-CN.*
[ ] Exception what() satisfies the What/Where/How recipe
[ ] detail/testing namespaces stay in their lanes: detail does NOT appear in public signatures
[ ] Consistent with the relevant L-N in lifecycle.md; if there's a conflict, fix lifecycle.md first
```

---

## 7. Document governance

Style adjustments MUST flow as **doc change → code change → test
change**; the reverse is not allowed (avoids "code drifts first,
docs catch up later").
