# Changelog

All notable changes to **aria** are documented here.

> Aria is an internal / not-yet-released project. We deliberately keep the
> version pinned at **1.0.0** and do **not** publish per-feature semver
> bumps. This file is therefore not a historical changelog in the usual
> sense — it is a snapshot of *what the framework currently is* plus a
> rolling **TODO** list of what is still wanted.

---

## 1.0.0 — current snapshot

Aria is a modern C++20 MVVM framework — cross-platform, layered,
coroutine-first. Everything below is implemented, tested and shipped in
the current tree.

### 2026-06-09 — submodule-friendly build + HTTP escape hatch

Aria is now safe to consume via `add_subdirectory()` from a parent CMake
project that:

* uses its own `CMAKE_INSTALL_PREFIX`,
* builds (or finds) its own OpenSSL,
* wants to register custom REST routes on aria::http's listening socket.

Concrete changes:

* All references to `CMAKE_SOURCE_DIR` in Aria's own CMake files were
  replaced with `PROJECT_SOURCE_DIR` (`CMakeLists.txt`,
  `cmake/BuildOpenSSL.cmake`, `cmake/PackageRelease.cmake`,
  `modules/adapters/http/CMakeLists.txt`). When Aria is the top-level
  project the two variables are identical; when Aria is a submodule the
  former mis-resolves to the parent project's root and breaks
  `third_party/{openssl,cpp-httplib,nlohmann_json}` paths.
* `cmake/BuildOpenSSL.cmake` now early-returns when the parent project
  has already defined an `openssl_external` target, instead of aborting
  with a duplicate-target error. The early-return is contractual — if
  the parent provides the target it MUST also expose
  `OPENSSL_INCLUDE_DIR` plus `OPENSSL_LIBRARIES` (or
  `OPENSSL_SSL_LIBRARY`/`OPENSSL_CRYPTO_LIBRARY`) so consumers like
  `find_package(OpenSSL)` keep working. Failing to do so is a hard
  configure error with a helpful message.
* `cmake/PackageRelease.cmake` is now only included when
  `PROJECT_IS_TOP_LEVEL` is true. Previously it would
  `set(CMAKE_INSTALL_PREFIX ... CACHE PATH "" FORCE)` even as a
  submodule, silently hijacking the parent project's install prefix.
* `aria::http` exposes `HttpAdapter::native_server() -> httplib::Server&`,
  a strongly-typed escape hatch for consumers that want to register
  their own REST routes / static mounts / chunked responses on the same
  listening socket. The cpp-httplib include is now PUBLIC over
  `BUILD_INTERFACE` only — installed consumers still see the
  forward-declared `httplib::Server` in the header but cannot reach the
  implementation, keeping the high-level API the canonical surface.
  Stability of `native_server()` follows `cpp-httplib`'s.

### 2026-06-08 — HTTP/REST/SSE adapter

A new platform adapter — `aria::adapters::http::HttpAdapter` — landed
under `modules/adapters/http/`. It is the **fourth production adapter**
alongside Qt6, AppKit and UIKit, and lets a single ViewModel drive a
browser frontend over a small JSON REST + Server-Sent-Events protocol
without leaving the `IViewAdapter` contract.

* **HTTP via `cpp-httplib`** (Yuji Hirose, MIT, vendored single-header).
  Routing, chunked SSE streams, worker thread pool — all from upstream.
* **JSON via `nlohmann::json`** (Niels Lohmann, MIT, vendored
  single-header). Fuzzed, UTF-8-correct, line/column-precise parse
  errors.
* **HTTPS via vendored OpenSSL 3.3.x** (Apache-2.0). Built from source
  by `cmake/BuildOpenSSL.cmake` so deployments don't depend on the
  user's system OpenSSL. TLS 1.2 / 1.3, optional mTLS via client CA
  bundle. Configurable via `HttpAdapterConfig::tls_cert_file` /
  `tls_key_file` / `tls_ca_file` / `tls_min_version`. Disable with
  `-DARIA_HTTP_ENABLE_TLS=OFF` for HTTP-only builds.
* **Vanilla-JS SDK** — `modules/adapters/http/web-sdk/aria_client.js`,
  a ~250 LOC pure-ESM module that works in any modern browser without
  a build step.
* **Bidirectional binding** — server-side `Property<T>` changes push
  via SSE; browser-side updates land back through the standard
  `on_text_changed` / `on_click` etc. callbacks.
* **`examples/4-web-mvvm`** — working bidirectional demo with a
  counter + greeting + reset, exercising state push, click events,
  custom commands, and HTTPS.
* **RFC** — `docs/rfc/0001-http-adapter.md` documents the protocol
  design, threading model, security defaults, and decision log.

WebSocket support is intentionally out of scope: the SSE protocol
covers >95% of the "ViewModel state push" use case, and the adapter's
abstraction is deliberately closed-set so downstream consumers don't
have to choose between transports.

### Latest framework-grade hardening (P1 capability completion pass)

This pass turns five P1 tracks into numbered, contract-bound primitives
that join the framework's promise surface. Each item is referenced by
ID from the canonical docs so a failing assertion can be traced to its
authoritative description.

* **P1-A `Loadable<T>` standard loadable view-model** — a
  five-state sum type (`Idle / Loading / Refreshing / Success /
  Error`) at par with SwiftUI `Task.Result` / Compose `LoadState`.
  Lives in `aria/loadable.hpp` (LO-1..LO-6) and is wired into
  `AsyncResource` via a derived `Property<Loadable<T>>` written
  synchronously at every state-mutation point. Pinned by 15 LO-N
  test cases plus four `AsyncResource <-> Loadable` interconversion
  cases (success / error+SWR / clear -> Idle / refresh -> Refreshing).

* **P1-B `AsyncValidator<T>` first-class async validation** —
  V-1 latest-wins, V-2 pending semantics, V-3 cancellation never
  surfaces as Error, V-4 `ValidationKey + rule_id`, V-5 identical-
  value de-duplication, V-6 lifetime cancel-on-detach. Lives in
  `aria/async/async_validator.hpp`. Drive-by fix: `CancellationSource`
  destructor is now safe on a moved-from instance (was a SIGSEGV
  trigger in `state.cancel = CancellationSource{}` patterns). Pinned
  by `test_async_validator.cpp` (4 InlineExecutor V-N cases plus 4
  `MainThreadExecutor + ThreadPoolExecutor` interleaving cases that
  exercise stale-drop, cancellation-not-Error, destruction-cancels-
  in-flight, and validator-dies-before-rule-completes).

* **P1-C derived collections completion** — three new collections
  with rigorous, source-ordered incremental contracts:
  `DistinctList<T, Key>` (PD-1..PD-5, slot-id-ordered visible vector
  + per-key duplicate bag, O(N_visible) per derived event in the
  worst case, O(1) amortised on tail push), `PagedList<T>`
  (PG-1..PG-5, O(page_size) source updates and re-window),
  `GroupedList<T, Key>` (PGR-1..PGR-6, per-group inner
  `ObservableList<T>`, outer slot anchored to the seed item's source
  position). All three never silently fall back to `Reset` for a
  single source mutation. PD-2 / PGR-4 explicitly cover the
  middle-insert case: a new key / group inserted between two
  existing source positions lands at the corresponding derived /
  outer slot rather than being appended to the end. Pinned by 30
  PD/PG/PGR cases (4 of which are dedicated middle-insert / prepend
  / frozen-position regressions). Bench rows in
  `aria_bench_derived_list` at n=10⁵: DistinctList push_back tail
  new key ~3.2 µs, PagedList push_back outside window ~400 ns,
  PagedList page_index hop ~7.5 µs, GroupedList push_back into
  existing group ~900 ns.

* **P1-D performance baselines and complexity contract** —
  `docs/performance.md` (PERF-1..PERF-5) pins HONEST complexity
  bounds for every public reactive / list / validator / async /
  diagnostics / loadable / binding / ABI operation. PERF-2 hard
  rule: derived collections never silently emit `Reset` in response
  to a single source mutation; the worst-case envelope is
  O(N_visible) per derived event (matching `FilteredList` /
  `SortedList`). The baseline snapshot anchored at n=10⁵ is a
  PERF-5 regression threshold, not a complexity claim. Anti-pattern
  table covers the recurring performance traps (`source.snapshot()`
  in a hot path, missing `has_trace_sink()` gate, mutating in
  `Computed`, etc.).

* **P1-E Navigator protocol upgrade** — `binding/navigation.hpp`
  now carries N-1 `Presentation::Push` vs `Presentation::Modal` per
  entry with `dismiss_modal()`, N-2 typed result passing via
  `push_for_result<R>(...)` returning `std::shared_future<
  std::optional<R>>` and `dismiss_with<R>(value)`, N-3 a per-entry
  `CancellationSource` exposed by `top_token()` so navigating away
  cancels in-flight entry work, and N-4 deep-link routing via
  `register_route("users/{id}", factory)` + `route("users/42",
  opts)` with `RouteOptions{clear_stack, presentation}`. Existing
  `push / pop / replace / clear / pop_to_root` API is fully backwards
  compatible. Pinned by 7 N-N cases (modal vs push, dismiss_with
  type-mismatch fallback, navigator destruction resolves pending
  result to nullopt, deep-link clear_stack and Modal opts).

### Latest framework-grade hardening (P0 hard-bedrock pass)

This pass turned every "happy path API" into a numbered, contract-bound
invariant. Each item is referenced by ID from the canonical docs so a
failing assertion can be traced to its authoritative description.

* **P0-α.1 ValidationKey / field-path protocol** — every validator
  message is now keyed by `ValidationKey { field_path, rule_id }` and
  flows into an `aria::Error` (kind = `Validation`). `FormValidator`
  cross-field rules use empty `field_path` + `form_rule_<N>` defaults.
* **P0-α.2 Unified `aria::Error` / `ErrorKind` taxonomy** — eight-kind
  enum (`UserError / Validation / AsyncFailure / Cancellation /
  Timeout / BindingFailure / GraphCycle / InvariantViolation`),
  copyable value type, `Property<std::optional<Error>>`-friendly,
  `from_exception()` mapping and per-subsystem factories. Validator,
  AsyncCommand (`last_error` + convenience `last_error_message`) and
  AsyncResource (`error` + `error_message`) all converged onto it; the
  protocol is pinned in `docs/error-model.md` (E-N IDs) and verified
  by `test_error_model.cpp`.
* **P0-α.3 List diff contract + framework conformance** —
  `docs/list-diff-contract.md` (LD-N IDs) chair-rules `Insert /
  Remove / Replace / Move / Reset / ItemChanged` semantics; new
  `test_list_conformance.cpp` mirrors every event against
  `ObservableList::snapshot()` so any adapter has a ready reference
  to consume.
* **P0-β Lifecycle invariants** — `docs/lifecycle.md` collects every
  invariant the framework already promises (`L-N` IDs across graph
  thread affinity, subscription, computed dynamic deps,
  unsubscribe-during-emit, view-destroy drop, async cancel/dtor).
  Verified by `test_lifecycle_invariants.cpp` and stress-verified by
  the new fuzz suite.
* **P0-γ Unified diagnostics** — `aria::TraceEvent` +
  `aria::TraceSink` (`install_trace_sink()` / `clear_trace_sink()` /
  RAII `ScopedTraceSink`). Six subsystems now emit
  events on the same channel: reactive graph (`graph.flush.*`),
  AsyncCommand (`async_command.*`), AsyncResource
  (`async_resource.*`), Validator (`validation.*`), BindingEngine
  (`binding.*`), ObservableList / derived collections (`list.*`).
  Pinned in `docs/diagnostics.md` (D-N IDs); verified by
  `test_diagnostics.cpp`.
* **P0-δ API style audit** — `docs/api-style.md` (S-N IDs) collects
  the namespace, naming, error, lifecycle and async-entry style
  contracts; concept constraints + `static_assert` messages render
  the first IDE diagnostic at the call site instead of 30 layers of
  SFINAE.
* **P0-ε Reliability / fuzz suite** — seven framework-level fuzzers
  hammer the lifecycle invariants. Default modest run (50k
  iterations / fuzzer) finishes in ~1.5s; nightly runs override via
  `ARIA_FUZZ_ITERS=1000000` to hit the headline lifecycle.md
  numbers. Coverage:
  `fuzz_signal_unsubscribe_during_emit` (L-13),
  `fuzz_computed_dynamic_dep` (L-17),
  `fuzz_reactive_reentrant_set` (L-20),
  `fuzz_binding_view_destroy_race` (L-32),
  `fuzz_observable_list_mutation_storm` (L-31),
  `fuzz_cancellation_race` (L-36),
  `fuzz_async_command_dtor` (L-37). Reproducible via
  `ARIA_FUZZ_SEED=...`.

All P0 items above are **green**: 9/9 ctest suites pass, including the
new `fuzz_tests` (≥ 860k assertions on default settings).

### Core (reactive layer)

* `Property<T>` / `Computed<T>` / dependency `Graph` with topological
  flush, cycle detection and reentrancy-safe notification.
* `IProperty` type-erased view (`get_any` / `set_any` / `subscribe_any`
  / `type`) implemented by `Property<T>`, enabling cross-TU plugin
  scenarios with a measured ~5–7 ns vcall overhead vs template-direct.
* `ObservableList<T>` with range ops (`insert_range` / `remove_range`
  / `move`), `ListChangeKind::{Insert,Remove,Replace,ItemChanged,Move,Reset}`
  and O(1) `index_of`.
* Derived collections — `FilteredList<T>` / `SortedList<T>` /
  `MappedList<Source,Target>` — with strict incremental contracts
  (predicate / comparator / mapper changes never collapse to a full
  `Reset`; `ItemChanged` crossing a sort boundary emits `Move`).
* `GraphInspector` — flush-trace callbacks (enqueue / skip / execute /
  duration_us), full cycle path (`A -> B -> C -> A`), debug-time
  detection of `Computed` writes into `Property`, auto-named nodes.

### Async (coroutine layer)

* `IExecutor` interface with safety capabilities + `executor_traits`
  / concept; `InlineExecutor`, `ThreadPoolExecutor`,
  `MainThreadExecutor` (production-grade: owner affinity, recursive
  drain, CV blocking, `run_one`).
* `co_await schedule_on(executor)`, `co_await schedule_after(...)`.
* `with_timeout(task, deadline, OnTimeout::{Cancel,Fail})` — `Cancel`
  cooperatively unwinds via `CancellationToken`; `Fail` fail-fast
  resumes the parent and lets the inner run detached. Built on
  `detail::RaceSlot<R>` which uses a two-phase `try_claim → write
  result → publish → notify` protocol verified under TSan stress
  (70 011 publish paths, zero races).
* `when_all` / `when_any` / `when_any_cancellable` — race-aware,
  `await_suspend` race-window audited, losers either cooperatively
  cancel or detach safely.
* `AsyncCommand<R>` (and `AsyncCommand<void>`) — composition over
  inheritance, RAII `Invocation`, `latest_only` / `drop_if_running`
  policies, chainable `with_timeout(...)` / `with_retry(...)`,
  compile-time + run-time guards against the broken
  "InlineExecutor as graph + non-Inline worker" combination.
* `retry` / `retry_if` / `retry_with_backoff` (fixed / exponential).

### Binding

* `BindingEngine` with `ui_dispatcher` + `DispatchPolicy::{Direct,
  AlwaysPost, SmartMarshal}` for VM→View marshalling.
* `bind_text_oneway`, `bind_text_twoway`, `bind_command`,
  `bind_visibility`, `bind_enabled`, `bind_list`, …
* `IViewAdapter` numeric surface (int / int64 / uint64 / double /
  string) with overflow-saturating helper + `Logger::warn` on lossy
  conversion.
* `FormValidator` — aggregate validity / dirty / pending,
  cross-field rules, submit gate as a `Command<>` predicate;
  driven from a real Signup form in `examples/1-qt-showcase`
  (email / password / confirm-password / agree-to-terms /
  cross-field rule + submit gate).

### Adapters

* `aria::adapters::qt6` — `QtAdapter`, `QtView`, `QtDispatcher`,
  `ObservableListModel<T>` (full `QAbstractItemModel`), passes the
  full adapter conformance battery offscreen, including the
  optional numeric extras (`int64` / `uint64` via `QSpinBox`,
  `float` via `QDoubleSpinBox`) so coverage matches AppKit /
  UIKit one-for-one.
* `aria::adapters::appkit` — production-grade macOS AppKit adapter,
  pimpl + per-view-per-kind bridge cache, ARC + C++ subscription
  lifetime strictly synced; ships `appkit_conformance` ctest target.
* `aria::adapters::uikit` — production-grade iOS UIKit adapter,
  same architecture as AppKit; in-app conformance runner passes
  25/25 on iPhone 17 Pro Max.
* Top-level CMake options `ARIA_BUILD_QT6 / APPKIT / UIKIT / JNI /
  WASM`, all opt-in, default `OFF`. UIKit subdir has a second-layer
  `iOS sysroot` gate that fails configure on a macOS host.

### Examples

* `examples/1-qt-showcase` — flagship Qt6 showcase covering chat,
  search, cart, signup form, derived collections.
* `examples/2-macos-appkit-mvvm` — standalone Xcode project, native
  AppKit + Objective-C++, ViewModel wired through `BindingEngine`.
* `examples/3-ios-oc-uikit-mvvm` — standalone Xcode project, native
  UIKit + Objective-C++, ViewModel wired through `BindingEngine`,
  in-app conformance runner.
* `examples/inspector-demo` — hands-on tour of `GraphInspector`
  flush-trace and cycle-path diagnostics.

### Build / CI / docs

* CMake ≥ 3.20, C++20, hidden-by-default visibility,
  `compile_commands.json` exported.
* `aria::warnings` interface target; ASan / UBSan / TSan toggles.
* Bundled doctest under `third_party/doctest/`, no network fetch.
* `scripts/init-project.{sh,ps1}`, `scripts/build.{sh,ps1}`,
  `examples/1-qt-showcase/scripts/run.{sh,ps1}` for one-shot bootstrap.
* Nightly bench gate — every `bench_*` target reports `mean / p50 /
  p95 / p99` per row; `benchmark/thresholds.json` pins per-metric
  P99 ceilings (with a scale-aware ×1.5 / ×2 / ×3 head-room),
  `scripts/check-bench.sh` parses the run, fails CI on any breach,
  and `.github/workflows/nightly.yml` writes a green/red summary
  table into `GITHUB_STEP_SUMMARY`.
* **CI matrix completion (Sprint4-#4)** — three new gates land on
  top of the existing macOS / Ubuntu / Windows-MSYS2 jobs:
  * **Windows MSVC native** (`windows-msvc` in `ci.yml`) — Visual
    Studio 17 2022 + Qt 6.7.2 binary install on `windows-2022`,
    Release configure → build → ctest → install smoke. Mandatory
    for the project's "ABI-stable on Windows" claim.
  * **clang-tidy gate** (`clang-tidy` in `ci.yml`) — runs the
    project's `.clang-tidy` checks across every `modules/*/include/
    aria/**.hpp` with `--warnings-as-errors='*'` against
    `compile_commands.json`. PR-blocking.
  * **TSan nightly** (`tsan` in `nightly.yml`) — `ARIA_ENABLE_TSAN
    =ON` Debug build runs the full ctest matrix nightly with
    `TSAN_OPTIONS=halt_on_error=1`. Heavy enough to keep out of
    per-PR CI; visible enough to catch new races.
  Local mirrors: `scripts/init-project.{sh,ps1}` now seed five new
  VSCode tasks — `aria: configure (ASan+UBSan)` / `aria: ctest
  (ASan+UBSan)` / `aria: configure (TSan)` (mac/Linux only) /
  `aria: ctest (TSan)` (mac/Linux only) / `aria: clang-tidy (all
  headers)` — so a developer reproduces every CI gate from inside
  the IDE without remembering the exact `cmake -DARIA_ENABLE_*`
  invocations.
* `docs/architecture.md` covers the layered design, ABI stability
  policy and the coroutine race model. Doxygen build is opt-in.

---

## Backlog pointer

`CHANGELOG.md` is a snapshot of what is currently shipped. The working
backlog and deliberately deferred items live in
[`docs/ROADMAP.md`](docs/ROADMAP.md), which is the single source of
truth for roadmap decisions.

Keep this file focused on implemented capability. Do not duplicate the
roadmap here; duplicated TODO lists drift and create fake work.
