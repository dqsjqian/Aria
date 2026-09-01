# Changelog

All notable changes to **aria** are documented here.

> Aria is an open-source project (MIT License). We follow semantic
> versioning: `MAJOR` bumps on breaking changes, `MINOR` on
> backward-compatible additions. The `1.0.0` → `1.1.0` bump in 2026-08
> marks the project's first public release; `1.2.0` in 2026-09 carries
> documentation and CI-gate work with no API change. This file is a
> snapshot of *what the framework currently is* plus a rolling **TODO**
> list of what is still wanted.

---

## 1.2.0 — current snapshot

Aria is a modern C++20 MVVM framework — cross-platform, layered,
coroutine-first. Everything below is implemented, tested and shipped in
the current tree.

### 2026-09-01 — release 1.2.0

No API changes. This release is documentation honesty plus two gates that
now actually gate: the README no longer makes claims about other
frameworks, the clang-tidy baseline is enforced, and a set of async tests
no longer leaks abandoned coroutine frames on Linux.

### 2026-09-01 — test: release abandoned inner frames in with_timeout::Fail

A full Linux build with GCC 14 / Clang 20 under ASan+LSan found ~1.1 KB
leaked across 10-11 allocations in `async_tests`, present since the
initial commit. Reproduced here with macOS `leaks` (LeakSanitizer refuses
to run on macOS at all, which is exactly why CI never saw this): 11
leaks / 2080 bytes, the root being a `ROOT CYCLE` on the
`drive_inner_for_fail_` coroutine frame.

The diagnosis in the report — that `timeout.hpp` needs cleanup logic or a
`final_suspend` hook — is wrong, and a one-line probe shows why: appending
`vt.advance_by(500ms)` to the failing case drops it from 6 leaks to 0
without touching the framework. `Task::start_detached` frees the frame at
`final_suspend`, and `final_suspend` is only reachable if something
resumes the coroutine. Two tests parked their inner on a 500ms virtual
delay, advanced the clock to 100ms so the timer would win, and then let
the executor die — so the abandoned frame was never resumed, never
reached `final_suspend`, and was still allocated at exit. A production
scheduler keeps running, so it always gets there.

Fixed in the tests, where the defect is: `no-token factory + deadline` and
`parent-cancel beats deadline` now drive the clock past the inner delay
before the executor goes out of scope. Verified per-case (6 → 0 and 5 → 0)
and full-suite: 135/135 passing, 402 assertions, 0 leaks / 0 bytes.

Worth stating for the record: the ASan+UBSan gate lives only on the macOS
job, where LeakSanitizer is unavailable, and the Ubuntu jobs run no
sanitizers. Leak coverage on Linux is therefore still missing from CI —
this commit fixes the leak, not the blind spot.

### 2026-09-01 — docs: make the two intro examples explain themselves

Reader feedback: the "30 seconds" snippet was unreadable. `engine`
appeared from nowhere with no indication of where it comes from, and the
example never said which parts are ViewModel and which are View — the one
distinction the whole framework is built around.

Rewritten as two explicitly labelled halves: the ViewModel as a struct
with no UI header in sight (and a note that it runs under a console
test), then the View-side wiring as four numbered steps that start from
a concrete adapter, build the engine *from* that adapter, bind, and then
mutate only data. The adapter class names are the real ones
(`qt6::QtAdapter`, `uikit::UIKitAdapter`, `jni::JniAdapter`) — the first
draft of this text invented `Qt6ViewAdapter`, which does not exist.

The Hello-world snippet had the same problem in miniature: `auto sub =`
with no explanation, and nothing downstream ever using `sub`, which reads
like dead code. It is the opposite — it is the only thing keeping the
subscription alive. Now documented as an RAII lifetime handle, with what
happens if you drop it, plus an explicit `release()` demonstration. All
four behavioural claims in that comment (initial sync fires immediately;
updates follow; `release()` silences it; discarding the return value
detaches on the spot) were verified by compiling and running the snippet
rather than reasoned about — which is how the missing `"count = 0"` line
in the expected output was found.

### 2026-09-01 — README: drop the framework comparison table

External review flagged two factual errors in the "how it compares"
table; auditing the rest of it turned up four more. The table claimed Qt
has no reactive engine and only "manual `connect`" (Qt 6.0 ships
`QProperty` / `QBindable` with automatic dependency tracking), that Qt
needs a per-platform QML rewrite (QML is cross-platform), that Qt has no
Web story (Qt for WebAssembly has been a supported platform since 5.13),
and it referenced a Qt class named `QCoroutine` that does not exist — Qt
has no native `co_await` support at all; the de-facto answer is the
third-party QCoro.

The individual cells were fixable, but the table itself was the defect: 40
assertions about four evolving frameworks, each one refutable in seconds,
none of them load-bearing for a reader deciding whether Aria fits. A wrong
claim there costs more trust than a bug does — a bug is a mistake of
craft, a wrong claim about someone else's project is a mistake of
attitude.

Replaced with a "where Aria fits" section that states the one thing Aria
does and then lists its **costs** (C++20 floor, no widgets, template layer
needs recompiles, adapters are on you, young ecosystem), plus an explicit
poor-fit paragraph pointing at Flutter and Qt Quick for people who want
the UI to come along. Other frameworks are named only where they are the
right answer.

Also in this pass:

* `Tests 75+` and `Build` shields were stale hand-maintained numbers →
  replaced by the live CI workflow badge.
* The Chinese README still cloned `dqsjqian/aria.git` and `cd aria`;
  commit 120142c fixed only the English copy.
* `README.html` / `README.en.html` deleted along with
  `scripts/open-readme.sh`. They were hand-maintained mirrors of the
  Markdown — every doc change had to be made twice, and both copies
  carried the errors above. GitHub renders Markdown natively.
* `scripts/README.md` was missing `tidy-gate.sh` and
  `pick-ios-simulator.py`; `docs/reference/api-style.md` S-41 carried a
  stray internal annotation.

### 2026-09-01 — clang-tidy gate now enforced

The gate had been running in audit mode: with no baseline file on disk it
printed its 2289 findings and exited 0. `scripts/clang-tidy-baseline.txt`
now lands from the CI artifact, so the gate fails on new debt.

Two hazards found while closing this:

* CI installed `brew install llvm`, i.e. whatever is newest. It has since
  moved to 23.x, and running the same tree under 23.1.0 reports **59 NEW
  entries** — `readability-trailing-comma`,
  `cppcoreguidelines-explicit-constructor` and friends that 22.x does not
  have. That is not new debt, it is a new ruleset, and it would have
  reddened the gate on an unrelated commit. The LLVM major is now pinned
  to `llvm@22`, matching the baseline.
* Because a skew is still possible (a deliberate bump, a local run), the
  baseline records the version that generated it and the gate warns loudly
  when the majors differ instead of dumping an unexplained wall of NEW
  entries. Baseline comment lines are skipped by the comparison.

Verified: self-comparison is clean; injected NEW and GREW cases are both
caught; the gate exits 1 with the version warning under local 23.1.0; and
the CI 22.1.8 findings compare clean against the committed baseline.

### 2026-08-31 — error-model verification targets

`docs/reference/error-model.md` §7 was the last section in the tree still
headed **"P0-ε target mapping (TODO)"**. It named four fuzzers; `ls
modules/core/fuzz/` confirmed none of them existed. All four now do.

* **`fuzz_async_command_cancellation_no_error`** (E-20 clause 2) — the
  invariant is *negative*, the kind that rots without anything failing:
  when cancellation starts leaking onto `last_error`, the UI just grows a
  spurious red banner on every navigation. Clauses 1 and 2 also compose
  into something stronger than either alone — because clause 1 clears the
  error face on `execute()` entry, a cancelled invocation must leave it
  **clean**, not still showing the previous failure. An implementation
  that only implements "don't write on cancel" passes a naive test and
  fails this one. Random `succeed / fail / timeout / cancel` walks against
  one long-lived command, plus the `fail -> cancel` ordering pinned
  directly.

* **`fuzz_error_property_equality_gate`** (E-11 / L-21) — a retry loop
  producing ten identical failures must notify once, not ten times, even
  though each failure carries a fresh `exception_ptr`. The reference model
  spells out E-11's field list **without** calling `operator==(Error,
  Error)`: using the library's own operator would make the assertion a
  tautology, since mutating the operator would mutate the model with it.
  Verified by mutation — comparing `inner` inside `operator==` is caught
  now and was silently accepted by the first draft.

* **`fuzz_validator_field_path`** (E-22 clause 1) — `field_path` is an
  invariant of the *validator*, and it has four production sites (`rule` /
  `warning` / and both `end_pending` overloads). The caller-shaped overload
  is asymmetric on purpose: the framework backfills an **empty**
  `field_path` but must not clobber one the caller set. A one-line "always
  assign" breaks only that half, which is exactly what this fuzzer
  catches.

* **`fuzz_error_from_exception_table`** (E-13 / E-12) — the mapping is an
  ordered `catch` chain, and `expects_inner` follows the *factory* each
  branch calls rather than the exception's richness:
  `Error::user_error` has no `inner` parameter, so the two `UserError`
  branches drop the `exception_ptr`, while `catch (...)` forwards it — a
  bare `throw 42` ends up **with** an inner ptr. Counter-intuitive enough
  that "tidying" it would silently change observable behaviour.

Each fuzzer was reverse-verified by deliberately breaking the
implementation and confirming a real failure: swapping the
`out_of_range` mapping, clobbering the caller's `field_path`, leaking
cancellation onto the error face, and adding `inner` to `operator==` all
produce failures now.

`error-model.md` §7 is now **"Verification targets"** and documents the
non-obvious parts of each contract (the clause 1 + 2 composition, the
tautology trap in the equality model, the backfill asymmetry, the
factory-driven `inner` retention) so the next reader does not have to
rediscover them from the assertions.



`docs/reference/diagnostics.md` §7 was headed **"P0-ε target mapping
(TODO)"** and listed four verification targets. None of the four existed.
The trace-sink machinery — install/clear racing, exception swallowing,
scoped nesting, and the zero-overhead fast path — had no fuzzer and no
bench, while `lifecycle.md`'s seven `L-N` fuzzers were all present. That
gap mattered more after the race-trace work landed, since arbitration
events publish from timer and worker threads.

All four now exist and run in the ordinary suites:

* **`fuzz_trace_sink_install_race`** (D-21) — publisher thread against
  installer thread. Each sink owns a heap guard that flips on
  destruction, so a sink invoked after its own destruction becomes a
  counted failure instead of a read of freed memory that only ASan might
  notice. The race requires a swap landing *between* the snapshot load
  and the invocation, which no single-threaded test produces.

* **`fuzz_trace_sink_throw_swallow`** (D-22) — throws on a rotating
  schedule including a type that does **not** derive from
  `std::exception` (the contract is `catch (...)`), across all four
  publish overloads, and verifies the slot survives: an escaping
  exception must not clear the sink as a side effect.

* **`fuzz_trace_sink_scoped_nesting`** (D-23) — random-depth scope stacks
  with a bare `install_trace_sink` injected mid-stack, asserting which
  sink is exposed at every unwind step.

* **`aria_bench_trace_sink`** (D-24) — a comparison rather than an
  absolute number: the gated no-sink path must sit within noise of a bare
  `has_trace_sink()`, and the ungated variant must stay visibly above it,
  or the AD2 gating convention buys nothing. Measured on an idle
  M-series host: gate 7.9ns p99, fast path 8.5ns, ungated 10.2ns,
  installed sink 37.8ns — the fast path is 0.6ns above a bare gate,
  which is the D-24 claim quantified.

`aria_fuzz` now carries 17 cases / ~2.0M assertions. The four new bench
rows are registered in `benchmark/thresholds.json` (both the generic and
`macos-arm64` blocks) and in `scripts/check-bench.sh`, which the script's
own comment requires; `check-bench.sh` reports 12/12 metrics within
budget.

Writing D-23 corrected a wrong assumption rather than finding a
framework bug: the intuitive model ("a bare install stays visible for
every level deeper than where it happened") holds only when
`bare_at + 1 == depth - 1`. Verified against the implementation with a
throwaway probe; the fuzzer now computes the expectation per level and
the reasoning is recorded next to it.

Running the extended suite under the `asan` flavor also surfaced a
**pre-existing UB in `fuzz_async_command_dtor`**, unrelated to the new
work: the command was executed with `static_cast<int>(rng.u32())` over
the full 32-bit range, and the action body's `x * 2` overflowed for large
negatives. UBSan aborted, taking the whole fuzz binary down — so the
`asan` flavor had never been getting a clean fuzz run. The argument is
incidental to what that fuzzer races (dtor vs `execute`), so it is now
bounded.

### 2026-08-31 — UIKit table-source tests, and a roadmap correction

`UIKitTableSource` was the only one of the four list/table bridges with no
test at all — Qt6 had `test_list_model.cpp`, AppKit
`test_appkit_table_source.mm`, JNI now `test_jni_list_source.cpp` — so its
row arithmetic and its derived-list support were carried entirely by
review.

* **`test_uikit_table_source.mm`**: six cases covering every `ListChange`
  variant, `shared_ptr` identity preservation across a Move, out-of-range
  `at()`, `FilteredList`, `MappedList<Source, Target>` identity, and
  teardown detaching from both the table and the source. Registered as a
  binary separate from `test_uikit_conformance`, so a table regression
  names itself in CI instead of hiding inside "conformance".

* **CI**: the `uikit` job now builds and runs both simulator targets on
  the one booted device.

Verified in an actual iOS simulator (6/6 cases, 43 assertions), not merely
compiled; breaking `apply_move_`'s insertion index fails three of them.
No framework code changed — this is coverage for shipped behaviour.

The roadmap item this closes, *Derived collections → UI adapter wiring*,
turned out to be **already implemented**: all four adapters accept their
source through `requires ::aria::ListSourceOf<L, T>`, so every derived
list — including `DistinctList` / `PagedList` / `GroupedList`, which the
item never mentioned — already binds without a manual sync layer. The
entry was describing work that no longer existed. It is now in `Landed`
recording that, and `docs/ROADMAP.md`'s maintenance rule gained a
"check the code before working an item" clause, because a stale roadmap
claim invites re-implementing shipped API.

### 2026-08-31 — JNI list source (RecyclerView)

Qt6, UIKit and AppKit each shipped a list/table source; JNI shipped none,
so the documented Android workaround was to join list items with `"\n"`
on the C++ side and split them back in Kotlin — discarding item identity,
per-row diffing and selection.

* **`JniListSource<T>`** (`aria/adapters/jni/JniListSource.hpp`) accepts
  any `aria::ListSourceOf<L, T>` — the same concept the other three
  adapters consume — and maps `ListChange<T>` onto the RecyclerView
  vocabulary: `notifyItemInserted` / `notifyItemRemoved` /
  `notifyItemChanged` / `notifyItemMoved` / `notifyDataSetChanged`. Rows
  stay `std::shared_ptr<T>`, so identity survives the hop.

* **`JniRecyclerNotifier`** (`aria/adapters/jni/JniRecyclerNotifier.hpp`)
  is the JNI half: it holds a global ref to the managed
  `RecyclerView.Adapter`, resolves the `notifyItem*` method IDs once
  (reflection per notification would put JNI lookups on the scroll path),
  and exposes `sink()`. `valid()` reports a failed lookup instead of
  silently dropping updates.

The two are separate on purpose. RecyclerView.Adapter lives on the
managed side and cannot be held as a widget pointer the way Qt / UIKit /
AppKit hold theirs, so the notification target is a `NotifySink`
callable — which also means **`JniListSource` has no `<jni.h>`
dependency and its diffing runs in the ordinary host test suite**. The
new `jni_list_source` ctest target (registered under
`modules/core/tests/`, not the ANDROID-gated adapter tests) covers all
six change kinds, derived lists, identity preservation, out-of-range
access and detach-on-destroy. Previously the entire JNI adapter was
unreachable from a host build, so a list bridge added there would have
had zero executed assertions.

Scope matches the other three adapters: no new collection model, no
Compose-specific API, and no thread marshalling — Aria owns no looper
abstraction, so a producer that emits off-main wraps `sink()` in a
`Handler` post. Documented in `docs/guide/adapters/jni.md`, including the
contract that `notifyItemMoved` takes the raw `(from, to)` pair with no
Qt-style `+1` adjustment (pinned by test — it is the classic porting
bug).

### 2026-08-31 — race-aware async trace

`with_timeout`, `when_any` and `when_all` already arbitrated a race
internally, but published nothing: only `AsyncCommand` and
`AsyncResource` reached the `TraceSink`, so "which participant won, and
why did the others stop" was invisible to tooling that could already see
every reactive flush and binding dispatch.

* **Six arbitration events**, specified as **D-31.1** in
  `docs/reference/diagnostics.md`: `race_start` / `race_won` /
  `race_timeout` / `race_loser_cancel` / `race_parent_cancel` /
  `race_end`, under the existing `TraceCategory::Async`. No new category,
  no new payload field — `trace::Async`'s `source` names the combinator
  (`with_timeout` / `when_any` / `when_any_cancellable` / `when_all`) and
  `generation` carries the per-op number (participant count, winner
  index, or losers signalled).

* **Exactly one of `race_won` / `race_timeout` / `race_parent_cancel`
  fires per race** — they are the three mutually exclusive outcomes of
  one CAS, published from inside the already-taken branch. A losing
  participant that finishes later publishes nothing, so the event count
  does not grow with participant count.

* **`race_loser_cancel` is one event per race, not per loser**, with the
  number of losers signalled in `generation`. An N-way race stays O(1)
  events.

* `race_timeout` carries `Error::timeout(...)` and `race_parent_cancel`
  carries `Error::cancellation(...)`, consistent with D-12.

New internal header `aria/async/detail/race_trace.hpp` holds the publish
helper and the op / source spellings, so the `has_trace_sink()` gate
(AD2) exists once rather than at each of the ten arbitration points, and
a misspelled op is a compile error instead of an event no filter matches.

Zero cost when no sink is installed: every publish goes through the same
one-load-and-branch gate as the rest of the diagnostic protocol, asserted
by a no-sink case in `test_async_diagnostics.cpp`. No public API change
and no behavioural change to arbitration itself — the 129 pre-existing
async tests pass untouched.

### 2026-08-31 — deterministic `Container` teardown

Fixes a real, timing-dependent defect rather than adding surface area:
`runtime::Container` stored singletons in an `unordered_map` and released
them via `clear()`, so teardown order was **unspecified**. A service
holding a reference to another service could observe a destroyed
dependency, and whether it did depended on hash order — the worst kind of
bug to reproduce.

* **Teardown is now reverse registration order** in both `clear()` and
  `~Container()`. Register providers before consumers and the container
  will not hand a destroyed dependency to a destructor. Singletons and
  factories share one order (previously two independently cleared
  tables), and re-registering a type replaces the instance while
  **keeping its original position** — the value changed, the dependency
  order did not.

* **`~Container()` no longer relies on the default member wipe.** It runs
  the same ordered path as `clear()`; previously the destructor inherited
  `unordered_map`'s order even for callers that never called `clear()`.

* **Each value is destroyed with the internal mutex released**, so a
  service destructor may call back into the container (`resolve` / `has`
  / `register_*`) without self-deadlocking — the A11 hazard, one layer
  down. Entries not yet reached stay resolvable, so a consumer's
  destructor can still reach a provider registered before it.

No API change: no new type, no new method, no signature change. The
contract is **L-40** in `docs/reference/lifecycle.md`, and
`test_container.cpp` pins it with five cases — reverting to forward order
fails four and *deadlocks* the re-entrant-destructor one.

Out of scope, deliberately: full dependency-graph-ordered teardown, which
would require recording the resolution graph during construction. See
`docs/ROADMAP.md` → *Evaluated and declined*.

### 2026-08-21 — bindable derived values, adapter-owned views, adapter base

First pass over the 2026-08 field-review triage (see `docs/ROADMAP.md`
→ *Landed*). All additive; no existing API changed behaviour.

* **`Computed<T>` is bindable.** New concepts in `aria/concepts.hpp` —
  `ReadOnlyReactive`, `ReadOnlyReactiveOf<T>`,
  `ReadOnlyReactiveOptional` — describe "readable + observable", which
  both `Property<T>` and `Computed<T>` satisfy. Every one-way binder now
  accepts them: `bind_*_oneway` (text / bool / int / int64 / uint64 /
  float / double), `bind_visible`, `bind_enabled`,
  `bind_text_projected`, `bind_optional_text`,
  `bind_text_converted_oneway`. A derived display value no longer needs a
  mirror `Property` plus a hand-written `on_changed`.

  The shipped `Property<T>&` overloads were kept next to the new
  constrained templates, so exported symbols keep their ABI and a
  `Property` argument still selects the non-template overload. **Two-way
  binders remain `Property`-only** — a computed value has no write-back
  path, so `bind_text(computed, view)` stays a compile error, asserted
  statically in `test_binding_readonly_source.cpp`.

* **`BindingEngine::adopt(IView&, Subscription)`** hands an arbitrary
  subscription to the view's per-view bucket, released on view-destroy or
  engine teardown. This is the escape hatch for anything the typed
  `bind_*` surface does not cover, and it removes the "process-global
  subscription vector that never releases" workaround.

* **Adapters own the handle → `IView` mapping.**
  `QtAdapter::view_for(QObject*)`, `AppKitAdapter::view_for(NSView*)` and
  `UIKitAdapter::view_for(UIView*)` cache one wrapper per native handle,
  so repeated calls share a single subscription bucket. Qt evicts on
  `QObject::destroyed`; the ARC-based adapters retain their handle and add
  `release_view(handle)` for hosts that discard a control early. All
  three destroy cached views outside the adapter mutex, because view
  teardown re-enters the adapter's own bridge cleanup.

* **`ViewAdapterBase`** (`aria/binding/view_adapter_base.hpp`) — opt-in
  base defaulting all 25 `IViewAdapter` operations to the compliant L-39
  unsupported path (warn through `runtime::Logger` under the stable
  `<platform>_adapter` category, then return a safe default). A new adapter overrides `platform_name()` plus only what
  its toolkit supports. `report_unsupported` is `virtual`.
  `IViewAdapter` itself is unchanged.

* **`DispatcherExecutor` / `DispatcherScheduler`**
  (`aria/runtime/dispatcher_executor.hpp`) bridge `IDispatcher` to
  `IExecutor` / `IDelayedScheduler`. Previously these lived only inside
  the Qt demo, so every host copied them.

* **Diagnosable executor injection.** The three `AsyncCommand` executor
  errors now name the remedy, not just the violation, and the startup
  ordering constraint is contract **L-5b** in
  `docs/reference/lifecycle.md`: platform executors and timers must be
  installed before any `AsyncCommand`-owning view model is constructed.

* **Docs.** `docs/guide/binding.md`'s quick reference is now exhaustive
  (previously omitted the `bind_*_oneway` numerics,
  `bind_text_converted*`, `bind_view_lifetime` and `adopt`, and never
  mentioned `Computed`). Cookbook recipe 8 starts from `ViewAdapterBase`
  and states that the conformance battery ships with the public headers.

* **One flagship application instead of two demo suites.** User-facing
  integration scenarios now live only in
  [AriaTools](https://github.com/dqsjqian/AriaTools). The former
  `examples/` tree is removed from Aria; its contract value moved into
  module tests and `tests/acceptance/`: the TodoMVC derived-collection
  workflow is a core test, cross-DSO `IProperty` remains a real shared-
  library gate, and UIKit conformance runs as a standalone simulator
  target rather than inside a sample app.

* **UIKit conformance exposed two hidden integration defects.** The CMake
  target now links CoreGraphics explicitly, and its double-value callback
  reads both `UISlider` and `UIStepper` senders correctly instead of
  unconditionally treating every `UIControl` as a slider.

* **JNI managed-event ingress.** `JniAdapter::notify_*` completes the
  Java/Kotlin → C++ half of two-way binding. Android listeners retain native
  listener ownership and forward text/bool/numeric/click events through the
  same `JniView` wrapper used by `BindingEngine`; unbound channels are safe
  no-ops. The bridge snapshots shared ownership before emitting, so view
  teardown cannot invalidate an in-flight callback.

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
* **HTTPS via vendored OpenSSL 3.5.x** (Apache-2.0). Built from source
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
