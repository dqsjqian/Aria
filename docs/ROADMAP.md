# Aria Roadmap

> Single source of truth for what is still wanted, deliberately deferred,
> or explicitly out of scope. Aria is open source (MIT License); the
> framework version currently stays at `1.2.0`. This is a prioritised
> working list, not a release plan.

---

## Direction

Aria is a C++20 MVVM framework. Its value is the reactive core,
bindings, commands, collections, validation, async primitives, adapter
contracts, diagnostics, and the ability to share ViewModel code across UI
hosts.

The roadmap must not pull Aria into becoming a full UI toolkit,
application framework, plugin runtime, storage layer, or distributed
state system. Prefer small, verifiable work that improves real user
experience or prevents real regressions.

---

## Input: 2026-08 field feedback

The `Landed` and `Now` lists below are driven by a written review from an
application author who built a 15-module, 4-platform app
(Qt6 / UIKit / JNI+Compose) on top of Aria 1.1.0, plus a design study
comparing Aria's reversible-effect story against Cordis. The first pass of
that triage has shipped; what is left in `Now` is mostly documentation and
application guidance now owned by AriaTools.

Two findings reframed the triage and are worth stating up front, because
they change what the fix actually is:

- **Several reported "missing framework features" are shipped but
  undiscoverable.** `JniAdapter` already implements the full typed
  `IViewAdapter` contract and passes the shared conformance battery, yet
  the reviewer wrote a string-protocol JNI bridge by hand because the
  former Android demo did exactly that, and `docs/guide/adapters/jni.md`
  documented it as *the* pattern. A demo that bypasses the adapter it is
  supposed to demonstrate is a worse defect than a missing API.
- **Several "platform asymmetries" are demo asymmetries.** The reviewer
  contrasted Qt's `subs_attached_to(QObject*)` against a hand-rolled
  process-global keepalive on iOS. `subs_attached_to` was not framework
  API — it lived in the former Qt showcase's local helpers. Neither
  platform had it. The real gap was one missing `BindingEngine` entry
  point, not per-platform work; it shipped as `BindingEngine::adopt` plus
  a per-adapter `view_for`, and the demo helper is gone.

Items the review asked for that Aria will **not** absorb are recorded in
*Evaluated and declined* below, so they do not get re-proposed every time
a new application hits them.

### Maintainer self-review

Reacting to a report only fixes what one consumer happened to hit. Two
passes over the shipped surface — reading `IViewAdapter` against the four
adapters that implement it, and reading a real cross-platform view
top-to-bottom — turned up three ergonomic defects the reviewer did *not*
report, and they explain a good part of what the reviewer did report.

The diagnostic worth keeping: in that app's Qt tip-calculator view, the
binding block is nine `bind_*` calls and the surrounding hundred lines are
widget construction, `view_for()` wrapping, and label back-fill. **The
reactive core is not what costs users; the perimeter around it is.**
`Adapter authoring ergonomics` and `Adopt view-lifetime helpers into the
framework` (both in `Landed`) and `First-contact documentation` (still in
`Now`) come from that observation rather than from the review, and were
prioritised on the same footing.

---

## Landed — 2026-08 triage, first pass

Kept here (rather than deleted) because each entry answers a specific
finding in the review above; a reader comparing the two sections can see
what the response actually was. Entries promoted out of `Next` later in
the cycle are appended at the end and say so.

### Bind read-only reactive sources

`aria::ReadOnlyReactive` / `ReadOnlyReactiveOf<T>` /
`ReadOnlyReactiveOptional` in `aria/concepts.hpp` describe "readable +
observable", which both `Property<T>` and `Computed<T>` satisfy. Every
one-way binder now accepts them: `bind_*_oneway`, `bind_visible`,
`bind_enabled`, `bind_text_projected`, `bind_optional_text`,
`bind_text_converted_oneway`.

The shipped `Property<T>&` overloads were kept alongside the new
constrained templates, so existing symbols keep their ABI and passing a
`Property` still selects the non-template overload. Two-way binders stay
`Property`-only, and `test_binding_readonly_source.cpp` asserts the
*absence* of a viable `bind_text(Computed&, view)` overload so a future
refactor cannot silently widen it. The misleading `bind_text_projected`
comment was corrected in the same change.

### Public per-view subscription adoption

`BindingEngine::adopt(IView&, Subscription)` exposes the per-view bucket
that `bind_view_lifetime` already reached. A hand-written `on_changed`
now gets exactly the lifetime a real binding has — released on
view-destroy or engine teardown, whichever comes first.

### Adopt view-lifetime helpers into the framework

`view_for(handle)` moved into the adapters that own the `IView` subclass:
`QtAdapter::view_for(QObject*)`, `AppKitAdapter::view_for(NSView*)`,
`UIKitAdapter::view_for(UIView*)`. Each caches one wrapper per handle, so
repeated calls share a single subscription bucket.

The lifetime story differs per platform, deliberately: Qt evicts on
`QObject::destroyed`; the ARC-based adapters retain their handle and so
offer `release_view(handle)` for hosts that discard a control early.
Teardown in all three destroys cached views *outside* the adapter mutex —
`~AppKitView` fires `on_destroy`, which re-enters the adapter's own
bridge cleanup. `test_appkit_view_for.mm` pins this: reverting that
destructor to a single locked `clear()` deadlocks the suite.

The former demos shrank as intended — the Qt showcase lost
`subs_attached_to` and its `view_for` keepalive, and both the AppKit and
UIKit controllers lost their `std::vector<std::unique_ptr<…View>>`
members.

### Adapter authoring ergonomics

`ViewAdapterBase` (`aria/binding/view_adapter_base.hpp`) defaults all 25
operations to the compliant L-39 unsupported path — warn through
`runtime::Logger` under the stable `<platform>_adapter` category, then
return a safe default — so a new adapter overrides only `platform_name()` plus
what it genuinely supports. `report_unsupported` is `virtual` for hosts
that would rather throw during bring-up. `IViewAdapter` is unchanged and
the four first-party adapters still derive from it directly. Cookbook
recipe 8 now starts from the base and states that the conformance
battery ships with the public headers.

### Bridge `IDispatcher` to `IExecutor`

`aria::runtime::DispatcherExecutor` / `DispatcherScheduler` in
`aria/runtime/dispatcher_executor.hpp`. The former Qt showcase consumed
the framework versions and its local `App/Executors.h` was removed.

### Make the executor-injection contract diagnosable

The three `AsyncCommand` executor errors now name the remedy rather than
just the violation, and the ordering constraint is contract **L-5b** in
`docs/reference/lifecycle.md` (executors and timers before any
`AsyncCommand`-owning view model). No bootstrap class — see *Evaluated
and declined*.

### Complete the binding quick reference

`docs/guide/binding.md` now lists every shipped binding with its
direction and accepted source types, including the previously omitted
`bind_*_oneway` numeric family, `bind_text_converted*`,
`bind_view_lifetime` and `adopt`, plus the two rules that explain the
table (one-way takes any read-only source; two-way takes `Property`
only).

### First-contact documentation and flagship example boundary

`README.md` / `README.en.md` now put a complete `Property` → `Computed` →
`BindingEngine` example immediately after the AriaTools link, before the
comparison, architecture, and toolchain material. AriaTools is the single
flagship application; Aria keeps focused snippets and executable contract
tests rather than a second application suite.

### Correct JNI guidance

`docs/guide/adapters/jni.md` now separates the typed `JniAdapter` path for
addressable Android `View` objects from the Compose side-channel path and
includes a worked `BindingEngine` example. The host-side interface contract is
pinned in Aria; the end-to-end View-backed behavioral lab is owned by AriaTools.

### Retire application examples without losing verification

The former `examples/` tree is removed. Its contract value moved into tests:
TodoMVC became a core integration test, cross-DSO `IProperty` moved to
`tests/acceptance/cross_dylib_iproperty`, and UIKit conformance became an
independent simulator target. User-facing integration lives in AriaTools.

### UIKit simulator CI

The shared adapter conformance battery now builds and runs as
`test_uikit_conformance` inside an iOS simulator. Moving it out of the example
app exposed and fixed two real integration defects: the target omitted
CoreGraphics, and the double callback treated every sender as `UISlider`
instead of supporting `UIStepper` as advertised.

### Documentation state pass

README Markdown/HTML mirrors, architecture, guides, scripts, package layout,
CI, and this roadmap now agree: Qt6/AppKit/UIKit/JNI/HTTP are shipped opt-in
adapters; WASM and Swift/SwiftUI remain triggered work; application examples
live in AriaTools.

### Deterministic `Container` teardown

Promoted out of `Next` on 2026-08-31 — the only entry in this section that
came from maintainer self-review rather than the field report.

`Container` stored singletons in an `unordered_map` and destroyed them
via `clear()`, so teardown order was unspecified: a service holding a
reference to another service could observe a destroyed dependency, and
whether it did depended on hash order.

Teardown now walks a recorded registration order back-to-front, in both
`clear()` and `~Container()` — the destructor previously inherited the
`unordered_map` order for callers that never called `clear()`. Singletons
and factories share one order rather than being two independently cleared
tables, and re-registering a type replaces the instance while keeping its
original position.

Each value is destroyed with the container mutex released, so a service
destructor may re-enter the container without self-deadlocking (the A11
hazard one layer down), and entries not yet reached stay resolvable. No
new API: the change is contained in the existing implementation, with the
guarantee stated as contract **L-40** in `docs/reference/lifecycle.md` and
pinned by five cases in `test_container.cpp` — forward order fails four of
them and deadlocks the fifth.

Full dependency-graph-ordered teardown remains out of scope; see
*Evaluated and declined*.

### Race-aware async trace

Promoted out of `Next` on 2026-08-31.

`with_timeout`, `when_any`, `when_any_cancellable` and `when_all` already
arbitrated a race internally but published nothing, so "who won, and why
did the others stop" was the one async question tooling could not answer —
while every reactive flush and binding dispatch was already visible.

Six events under the existing `TraceCategory::Async`, specified as
**D-31.1** in `docs/reference/diagnostics.md`: `race_start` / `race_won` /
`race_timeout` / `race_loser_cancel` / `race_parent_cancel` / `race_end`.
No new category and no new payload field — `source` names the combinator
and `generation` carries the per-op number (participant count, winner
index, losers signalled).

The event count does not grow with participant count: exactly one of
`race_won` / `race_timeout` / `race_parent_cancel` fires per race (the
publish sites sit inside the already-taken CAS branch, and a late loser
publishes nothing), and `race_loser_cancel` is one event per race rather
than one per loser. The `has_trace_sink()` gate lives once in
`aria/async/detail/race_trace.hpp` instead of at each of the ten
arbitration points, which also makes a misspelled op a compile error
rather than an event no filter matches.

No DevTools UI, per the original note — the trace data is complete and
testable first. Six cases in `test_async_diagnostics.cpp` pin the op
spellings, the ordering guarantees, the `generation` semantics, and the
no-sink path; misspelling one op fails four of them.

### JNI list source

Promoted out of `Next` on 2026-08-31.

Qt6, UIKit and AppKit each shipped a list/table source; JNI shipped none,
and the reviewer's workaround was to join list items with `"\n"` and split
them back in Kotlin, which discards item identity, per-row diffing and
selection.

`JniListSource<T>` now consumes the same `ListSourceOf<L, T>` concept as
the other three and maps `ListChange<T>` onto
`notifyItemInserted` / `notifyItemRemoved` / `notifyItemChanged` /
`notifyItemMoved` / `notifyDataSetChanged`, with rows kept as
`std::shared_ptr<T>`. Scope stayed identical to the other three: no new
collection model, no Compose-specific API.

The one structural difference is forced by the platform and turned into
an advantage. RecyclerView.Adapter lives on the managed side and cannot be
held as a widget pointer, so the notification target is a `NotifySink`
callable rather than a handle — which means `JniListSource` needs no
`<jni.h>` and **its diffing runs in the ordinary host `ctest`**
(`jni_list_source`, registered under `modules/core/tests/`). Only
`JniRecyclerNotifier`, which drives `notifyItem*` through a real VM, stays
NDK-bound and static-asserted. Before this, the whole JNI adapter was
invisible to a host build, so a list bridge placed in it would have had no
executed assertions at all.

Thread marshalling was deliberately left out — Aria owns no looper
abstraction, and adding one would exceed an adapter's remit; a producer
that emits off-main wraps `sink()` in a `Handler` post, and the row is
resolved at emit time so a deferred sink still sees the right item.

### Derived collections → UI adapter wiring

Promoted out of `Next` on 2026-08-31 — **mostly as a correction**. The
entry asked to "make `FilteredList`, `SortedList`, and `MappedList` easy
to consume from existing list/table adapters"; auditing the four adapters
showed that had already shipped. All four take the source through
`requires ::aria::ListSourceOf<L, T>`, so every derived list — including
`DistinctList`, `PagedList` and `GroupedList`, which the entry did not
even mention — already binds with no manual sync layer. The roadmap was
describing work that no longer existed.

What the audit *did* find was a verification gap, which is the part that
needed doing: `UIKitTableSource` was the only one of the four bridges with
**no test at all** (Qt6 had `test_list_model.cpp`, AppKit
`test_appkit_table_source.mm`, JNI `test_jni_list_source.cpp`), so its row
arithmetic and its derived-list support were carried entirely by review.

`test_uikit_table_source.mm` closes that: six cases covering every
`ListChange` variant, `shared_ptr` identity across a Move, out-of-range
`at()`, `FilteredList`, `MappedList<Source, Target>` identity
preservation, and teardown detaching from both the table and the source.
It is a separate binary from `test_uikit_conformance` so a table
regression names itself in CI rather than hiding inside "conformance", and
the `uikit` CI job now builds and runs both simulator targets.

Verified in an actual iOS simulator, not just compiled: 6/6 cases, 43
assertions. Breaking `apply_move_`'s insertion index fails three of them.

---

## Now — small, concrete, high-confidence

These should be done before adding broad new surface area.

### Publish generated API reference

The Doxygen pipeline already exists (`ARIA_BUILD_DOCS=ON`,
`aria_docs`) and CI builds the docs artifact. The remaining work is
infrastructure only:

- publish the generated HTML somewhere stable (for example GitHub Pages
  or a release artifact link);
- link it from `README.md` and `docs/index.md`;
- keep the scope limited to the public API.

Do **not** build a custom docs site unless the generated reference proves
insufficient.

## Next — useful, but not urgent

These are real framework improvements, but should be scheduled only after
`Now` is clean.

### Common widget binding polish

Add narrowly-scoped bindings only when a real demo or workload hits the
need. The first likely candidate is:

- `QComboBox` two-way binding and option-list binding.

Avoid inflating `IViewAdapter` with broad drag/drop, clipboard, focus, or
layout APIs until a concrete adapter demo proves the gap.

---

## Triggered — do only when the trigger is real

### Swift / SwiftUI

Trigger: a real SwiftUI consumer or a clear requirement to ship SwiftUI
examples.

Start with a short spike before committing to surface area:

1. validate Swift/C++ interop against the existing templates,
   coroutines, `std::function`, and ABI-facing interfaces;
2. decide whether to use direct C++ interop, Objective-C++ bridges, or a
   dual-track API;
3. only then consider `modules/adapters/swift` and SwiftUI integration.

Do not implement S1-S6 as a speculative adapter program.

### WASM adapter

Trigger: a real in-browser C++ workload where the HTTP adapter is the
wrong shape (for example CAD, audio/video processing, ML, or other local
compute-heavy UIs).

The existing HTTP adapter already covers the common "C++ stays in a
process, browser is a thin UI" scenario. WASM should remain separate and
triggered.

### AppRuntime consolidation

Trigger: real pain from process-wide services — for example parallel
in-process tests interfering with each other, multiple independent app
roots in one process, or teardown isolation bugs.

Until then, `EventBus::global()`, the main dispatcher, and the logger are
acceptable as standalone services. If this lands, it must be a thin owner of
existing services, not a new application framework.

Note on the trigger: `Container` is **not** one of the process-wide services.
It has no `global()` accessor — every `Container` is explicitly instantiated
and owned by its caller — so the "parallel in-process tests interfering"
scenario cannot arise through it. The real shared state is
`EventBus::global()` and `set_main_dispatcher`, both of which rely on tests
calling `clear()` for isolation today.

### ObservableList slot identity

Trigger: a real workload needs the same object instance to appear in
multiple independent logical rows and the documented workaround (distinct
`shared_ptr<T>` instances for distinct rows) is not acceptable.

The current duplicate-`shared_ptr` behaviour is intentional and tested;
do not change it speculatively.

### Cross-toolchain ABI expansion

Trigger: real plugin or binary-adapter pressure beyond the existing
`IProperty<T>` smoke.

The `tests/acceptance/cross_dylib_iproperty` acceptance test is enough for
current validation. Do not turn it into a plugin framework.

### Enum two-way binding

Trigger: a real adapter demo where the documented workaround is
insufficient.

`IViewAdapter` speaks `int`; a `Property<SomeEnum>` therefore cannot bind
to a stepper or combo box directly. The reviewer's workaround was one
`Command<>` per enum value.

Today the answer is `bind_text_converted` with a
`Converter<SomeEnum, int>`-style conversion, which is why this is not in
`Now`. If a demo shows the converter path is genuinely inadequate, the
narrow fix is an enum-aware scalar binding that casts through the
underlying type — **not** widening `IViewAdapter` with an enum channel.

### Write-provenance in the inspector

Trigger: a debugging session where `TraceSink` plus `GraphInspector` are
demonstrably insufficient to answer "who wrote this property".

`GraphInspector` already emits flush events (`Pull` / `Recomputed` /
`SkipClean`) and DOT/JSON graph dumps, and `callback_boundary` already
reports cross-thread violations. Attributing each individual
`Property<T>::set` to a call site would be a genuine differentiator, but
it costs a `source_location` per write and a diagnostics-only storage
path.

Build it only against a concrete case the shipped tooling could not
close, and keep it `NDEBUG`-gated with zero release cost.

---

## Evaluated and declined

Requests from the 2026-08 review that Aria will not absorb. Each is a
real problem in the application that reported it; none belongs in an MVVM
framework. Recorded here so they are answered once rather than
re-litigated per consumer.

### Built-in i18n / string catalogue

Requested: a framework `text(prop, key)` that binds a property to a
translation key, infers the module from `std::source_location`, and
re-runs on language change.

Declined. A string catalogue is application configuration — file format,
key namespacing, module ownership, fallback policy, pluralisation rules.
Aria supplies the reactive substrate this is built from: model the locale
as a `Property`, derive every label as a `Computed` that reads it, and
`batch` the switch. That is `docs/cookbook/05-theme-locale-switching.md`,
and it is the same mechanism the reviewer's own helper is implemented on
top of.

Owning a catalogue means owning XML/JSON parsing, a resource-loading
path, and a key-validation story — the "generic application framework
features" this roadmap already rules out. Extend the cookbook recipe to
cover the source-location trick and a per-module key convention instead.

### Module system, `IModuleLoader`, scaffolding CLI, build CLI

Requested: dynamic module loading via `dlopen`, registry-driven module
routing, `wb new-module <name>`, and `wb build --platform ...`.

Declined — every item is on the existing `Won't do` list (module systems,
plugin runtime, generic application framework). A module system is the
defining feature of the application shell that sits *above* Aria, and it
is reasonable for that shell to own it. The C++ side of dynamic loading
is also already addressed at the layer that matters:
`tests/acceptance/cross_dylib_iproperty` proves `extern "C"` +
`IProperty<T>` type erasure crossing a `dylib` boundary, which is the
ABI-safe primitive. Composing modules on top of
that primitive is application work.

Toolchain probing (`vswhere`, `xcode-select`, NDK paths) is likewise
per-project; Aria ships CMake targets, not a build front-end.

### Application bootstrap orchestrator

Requested: a `CoreBootstrap` (or equivalent) that encodes the correct
startup sequence — inject executors, construct the core, load modules,
register views.

Declined as designed, accepted as documentation. The underlying
constraint is real and is now documented as contract L-5b in
`docs/reference/lifecycle.md`: executors must be installed before
`AsyncCommand`-owning view models are constructed. But the sequence the
request describes includes module loading and view registration, neither
of which Aria owns — the class would have to reach into application
concepts to be useful, and that is how a framework becomes an
application framework.

Aria states the ordering contract; the host enforces it.

### Prescriptive binding schema

Requested: constrain binding direction via VM-declared metadata (a
`BINDINGS` block) so juniors cannot pick the wrong direction.

Declined. Direction is already encoded in the API surface —
`bind_text_oneway` versus `bind_text`, and since the read-only-source
work landed a `Computed` source makes two-way binding a compile error
rather than a convention. A parallel declaration layer would need macros
or codegen,
duplicate information the call site already carries, and can drift out of
sync with it.

If the naming is the actual complaint, fix the naming and the reference
table (the table is now exhaustive — see `docs/guide/binding.md`) — not
the binding model.

### Widget factories and control creation

Requested: framework-provided control constructors — the review lists
`make_label`, `make_stack_vc`, `make_button` and similar under "should be
built into the framework", having written a `platform/ios/support/` layer
of them.

Declined. This is UI toolkit surface, and "not a UI toolkit" is the first
constraint in *Direction*. Aria binds to views it did not create; the
moment it constructs and styles them it owns layout, theming, and
per-platform control catalogues for four platforms.

The legitimate part of that request — wrapping an existing native handle
as an `IView` without hand-writing the adapter glue — is accepted and
scheduled as *Adopt view-lifetime helpers into the framework*. Creating
the control stays with the application; adapting it belongs to Aria.

### Unified view-model base to replace host wrappers

Requested: a single framework VM base class, because the reviewer's
plain-`ViewModel` types could not satisfy their module contract and had
to be wrapped in `HostVm` shims (five of them).

Declined. `ViewModel` already is the framework base — it carries
lifecycle, `add_child` cascade, destroy hooks, and `bag()`. What those
types could not satisfy was the *application's* `IModule` contract, which
Aria does not define and should not. A framework base cannot be shaped to
fit an interface it cannot see.

If the application's module contract requires more than `ViewModel`
provides, that is an application-side base class deriving from
`ViewModel` — which is what `HostVm` already is, and it is the right
layering.

### Qt stylesheet / dark-theme guidance

Requested: framework guidance and helpers for Qt widgets that become
unreadable under a dark system palette (a light `background` set without
an explicit `color`).

Declined. This is Qt stylesheet behaviour, not Aria behaviour — Aria never
sets a colour and has no theme model. Correct fix belongs upstream in Qt
documentation or the application's own style layer. The reactive half of
theme switching is already covered by cookbook recipe 5.

### `EventBus` single-instance enforcement

Requested: forbid member-held `EventBus` instances, or assert that any
instance is the global one, after two subsystems each held a different bus
and cross-module events silently vanished.

Declined as an enforcement mechanism. `EventBus::global()` exists and is
documented; a per-instance bus is a legitimate choice for tests and for
isolated subsystems, and `AppRuntime consolidation` (already Triggered)
is where process-wide service ownership gets revisited. Asserting
`this == &global()` would break in-process test isolation, which is the
main reason non-global instances exist.

The failure mode — a publisher and a subscriber holding different buses —
is worth a documented warning next to `global()`. Enforcement is not.

### Event-payload design guidance in framework docs

Requested: framework-level guidance that cross-module events should carry
a full state snapshot rather than a delta, after a subscriber-side
accumulator drifted (increments applied, removals missed).

Declined for the framework reference; the diagnosis is correct but it is
distributed-state design, not an Aria contract. Aria's own contracts
(`docs/reference/lifecycle.md`) describe framework guarantees, and adding
general messaging advice there dilutes them.

Suitable as a short cookbook note if `EventBus` recipes are ever
expanded.

### Cordis-style `Scope` tree

Requested (design study): a nestable `Scope` type that tracks reversible
effects, destroys children before parents, and cancels coroutines
attached to it.

Declined — Aria already has all three properties, assembled differently:

- reversible effect → `Subscription` (RAII);
- aggregate release, reverse order → `SubscriptionBag` (contract L-12);
- child-before-parent teardown → `ViewModel::add_child` + the destruction
  order in contract L-34;
- coroutine cancellation on owner destruction → `ViewModelScope`, which
  cancels *and joins* with a bounded timeout and reports stuck coroutines
  as leaks (contract L-35/L-36).

A `Scope` type would be a fourth spelling of ownership alongside
`Subscription`, `SubscriptionBag`, and the `ViewModel` tree. Two of the
three concrete gaps the study cited are answered by smaller means —
`BindingEngine::adopt` for view-scoped subscriptions (landed), ordered
`Container` teardown for service ordering (`Next`) — without adding a
competing lifetime primitive.

### Type-safe service registry (beyond ordering)

Requested (design study): an IoC registry that resolves by C++ type and
tears down in dependency order.

Declined as new API. `runtime::Container` already registers and resolves
by type with no strings and no reflection; its one real defect is
unspecified teardown order, which is scheduled in `Next`. Full
dependency-graph-ordered teardown requires recording the resolution graph
during construction — meaningfully more machinery than reverse
registration order, for a failure mode not yet observed in practice.

Revisit only if reverse-registration-order teardown proves insufficient
against a real case.

---

## Won't do

These remain outside the framework unless a concrete product requirement
forces a re-evaluation:

- cross-process / cross-machine reactive graph;
- automatic `Property<T>` persistence or state restoration;
- heavy declarative UI DSL;
- generic application framework features such as configuration,
  persistence, RPC, service hosting, module systems, or DI beyond the
  existing `Container`;
- full plugin runtime;
- broad ABI-stable refactors beyond proven binary-boundary needs;
- C++23 reflection / `std::meta` auto-binding before at least two major
  compilers ship production-ready support.

---

## Maintenance rule

A roadmap item should stay only if it has at least one of these:

1. a failing or missing user-visible workflow;
2. a real regression risk that tests / CI can close;
3. a documentation or packaging gap that blocks adoption;
4. a concrete external trigger.

Otherwise it is a distraction and should be moved to `Won't do` or
removed.

**Check the code before working an item.** *Derived collections → UI
adapter wiring* sat in `Next` after the work had already shipped — all
four adapters took `ListSourceOf` — so the roadmap was asking for
something that existed. An item is a claim about the tree, and a stale
claim is worse than no entry: it invites re-implementing shipped API. If
an audit shows an item is already done, the correct action is to move it
to `Landed` saying so, and to record whatever the audit *did* turn up
(here: the untested UIKit bridge) as the real remaining work.
