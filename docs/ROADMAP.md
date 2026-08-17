# Aria Roadmap

> Single source of truth for what is still wanted, deliberately deferred,
> or explicitly out of scope. Aria is open source (MIT License); the
> framework version currently stays at `1.1.0`. This is a prioritised
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

The current `Now` list is driven by a written review from an application
author who built a 15-module, 4-platform app (Qt6 / UIKit / JNI+Compose)
on top of Aria 1.1.0, plus a design study comparing Aria's reversible-
effect story against Cordis.

Two findings reframed the triage and are worth stating up front, because
they change what the fix actually is:

- **Several reported "missing framework features" are shipped but
  undiscoverable.** `JniAdapter` already implements the full typed
  `IViewAdapter` contract and passes the shared conformance battery, yet
  the reviewer wrote a string-protocol JNI bridge by hand — because
  `examples/5-android-jni-mvvm` does exactly that, and
  `docs/guide/adapters/jni.md` documents it as *the* pattern. A demo that
  bypasses the adapter it is supposed to demonstrate is a worse defect
  than a missing API.
- **Several "platform asymmetries" are demo asymmetries.** The reviewer
  contrasted Qt's `subs_attached_to(QObject*)` against a hand-rolled
  process-global keepalive on iOS. `subs_attached_to` is not framework
  API — it lives in `examples/1-qt-showcase/App/UiHelpers.h`. Neither
  platform has it. The real gap is one missing `BindingEngine` entry
  point, not per-platform work.

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
Everything in `Adapter authoring ergonomics`, `First-contact
documentation`, and `Adopt view-lifetime helpers into the framework`
below comes from that observation rather than from the review, and is
prioritised on the same footing.

---

## Now — small, concrete, high-confidence

These should be done before adding broad new surface area.

### Bind read-only reactive sources

`BindingEngine` takes `Property<T>&` everywhere, so a `Computed<T>`
cannot be bound at all — despite the `bind_text_projected` comment block
in `modules/binding/include/aria/binding/binding_engine.hpp` explicitly
advertising "a `Computed`'s formatted output" as a supported case. Every
derived display value therefore falls back to a hand-written
`on_changed` plus a caller-owned subscription, which is what pushed the
reviewer into the global-keepalive workaround.

`Computed<T>` already exposes `get()` / `on_changed()` — the same shape
the engine needs. The work is to bind against that shape instead of the
concrete `Property<T>`:

- introduce a read-only reactive-source concept covering `Property<T>`
  and `Computed<T>`;
- accept it in the one-way bindings (`*_oneway`, `bind_text_projected`,
  `bind_optional_text`, `bind_visible`, `bind_enabled`);
- leave two-way bindings `Property`-only — a computed value has no
  write-back path, and that should stay a compile error.

Fix the misleading comment in the same change.

### Public per-view subscription adoption

The engine already owns a per-view subscription bucket keyed on
`IView::on_destroy`, and `bind_view_lifetime` already reaches it — but
only to register a teardown callback. There is no way for a caller to
hand an arbitrary `Subscription` to that bucket, so anyone who writes a
manual `on_changed` has to invent their own storage. Both the Qt demo
(`subs_attached_to`) and the reviewer's iOS layer (a process-global
`std::vector<Subscription>` that never releases) exist purely to fill
this hole.

Expose the existing bucket:

```cpp
void adopt(IView& view, Subscription s);   // released on view-destroy
```

This is a small addition over machinery that already ships, it removes
the leak class outright, and it lets the Qt demo drop its own helper.

### Adapter authoring ergonomics

*(Maintainer self-review — not raised in the field report.)*

`IViewAdapter` declares 25 pure virtuals: text, bool, int, int64, uint64,
float, double each with `set_` / `get_` / `on_*_changed`, plus
`set_visible`, `set_enabled`, `on_click`, `platform_name`. There is no
base class. An author who only needs text and click must still write all
25, and there is no compile-time help toward the L-39 contract — every
unsupported path must hand-roll its own `warn_unsupported_`, exactly as
`docs/cookbook/08-writing-a-view-adapter.md` spells out by hand.

This is a real barrier to the framework's central promise. "Bring your
own UI host" is only credible if hosting costs an afternoon, and today
the floor is 25 methods regardless of ambition.

Add an opt-in base that defaults every operation to the compliant
unsupported path (warn via the callback boundary, return a safe default),
so an adapter overrides only what it genuinely supports:

- keep `IViewAdapter` unchanged — it is ABI-stable and shipped;
- add the base alongside it, no behavioural change to existing adapters;
- migrate the cookbook recipe to start from the base and shrink to the
  operations being demonstrated.

Related: the `adapter_conformance` battery is already installed with the
public headers, so third-party adapters can verify themselves — this is
shipped and only needs to be stated in the recipe.

### First-contact documentation

*(Maintainer self-review — not raised in the field report.)*

`README.md` reaches its first line of Aria code at **line 260**, under
"Hello, world". Ahead of it: a competitor comparison table, a
ten-module architecture diagram, environment requirements, build
scripts, a Windows toolchain section, and build options. A reader
evaluating whether the reactive model suits them has to scroll past all
of it.

The field report's "all my API knowledge came from reading source" has
the same root as the `jni.md` defect: the material exists and is not
positioned where a newcomer meets it.

- lead with the smallest complete `Property` → `Computed` → binding
  example, then the comparison and architecture material;
- link `docs/guide/binding.md` and the cookbook from that example, since
  both already answer the follow-up questions;
- keep the build and toolchain sections — move them below first contact.

Cheap, and it compounds with every other documentation item here.

### Adopt view-lifetime helpers into the framework

*(Maintainer self-review — generalises one reported symptom.)*

Both shipped GUI demos independently grew the same two helpers:
`view_for(native_handle)` to wrap a platform object as an `IView`, and a
per-owner subscription store. The reviewer's app grew a third and fourth
copy (`QtViewFactory`, `IosUi`), and its iOS copy leaks by construction.

When every consumer writes the same wrapper, the framework has the wrong
seam. `BindingEngine::adopt` above fixes the subscription half. The
`view_for` half belongs in each platform adapter, which already owns the
`IView` subclass and its handle-to-view cache:

- give each adapter a documented `view_for`-equivalent entry point;
- have the Qt and UIKit demos consume it and delete their local copies —
  demos shrinking is the acceptance criterion.

Explicitly **not** in scope: widget factories (`make_label`,
`make_stack_vc`). Those are UI toolkit surface and stay in application
code — see *Evaluated and declined*.

### Bridge `IDispatcher` to `IExecutor`

`IDispatcher` derives from `IDelayedScheduler`; `IExecutor` is a separate
branch. Anything that needs a UI executor — `AsyncCommand` most of all —
cannot take a platform dispatcher directly, so every host writes the same
two forwarders. Aria ships them only inside a demo
(`examples/1-qt-showcase/App/Executors.h`), which means each new
application either copies them or rediscovers the need.

Promote them to framework API next to `IDispatcher` (they are
dispatcher-generic, not Qt-specific), and have the demo consume the
framework version.

### Make the executor-injection contract diagnosable

Constructing an `AsyncCommand` before a real UI executor is installed
fails with:

> `AsyncCommand: cannot use InlineExecutor as the graph-thread executor
> when worker runs on a different thread.`

The statement is accurate and tells the reader nothing about what to do.
Two cheap fixes:

- rewrite the message to name the remedy (install a main-thread
  `IExecutor` — e.g. the dispatcher bridge above — before constructing
  `AsyncCommand` view models);
- document the ordering constraint in `docs/reference/lifecycle.md`:
  platform executors and timers must be installed **before** any
  `AsyncCommand`-owning view model is constructed.

Document the constraint; do not add a bootstrap/orchestrator class to
enforce it (see *Evaluated and declined*).

### Correct the JNI adapter guidance

`examples/5-android-jni-mvvm` does not use `JniAdapter`. Its bridge
pushes every property through `onPropertyChanged(String, String?)` into a
`Map<String, String>` StateFlow, and `docs/guide/adapters/jni.md`
presents that side-channel as the Android architecture. Readers
reasonably conclude Aria has no typed Android binding, and rebuild the
string protocol at application scale — losing type safety, list
structure, and compile-time command checking in the process.

The adapter is real and conformance-tested. The documentation is wrong
about it. Required:

- state in `jni.md` that `JniAdapter` is the typed, supported path for
  Android `View`-backed UI, with a worked `BindingEngine` example;
- keep the side-channel pattern documented, but scoped honestly: it is
  the answer for **Compose**, which has no addressable view object to
  bind to — not a general substitute for the adapter;
- update the demo to bind at least one screen through `JniAdapter` so the
  example proves the claim.

This is documentation and example work, not new framework surface, and it
is the highest-leverage item in this list.

### Complete the binding quick reference

The `docs/guide/binding.md` quick-reference table omits shipped API
(`bind_text_converted`, `bind_float*`, `bind_view_lifetime`) and never
mentions `Computed` at all. The reviewer read adapter and engine headers
to reconstruct the surface. Once the read-only-source work lands, make
the table exhaustive and state the direction and accepted source type for
every entry.

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

### Documentation state pass

Keep shipped/planned status consistent across:

- `README.md` / `README.en.md` and their tracked HTML mirrors;
- `docs/architecture.md`;
- `docs/index.md`;
- this roadmap;
- the backlog section in `CHANGELOG.md`.

The important statuses today are:

- shipped first-class adapters: Qt6, AppKit, UIKit, JNI, HTTP;
- planned / triggered only: WASM, Swift / SwiftUI;
- explicitly out of scope: declarative UI DSL, persistence, distributed
  reactivity, full plugin framework, generic application framework.

### Larger real-application case study

Add one non-trivial example or written case study that exercises forms,
list filtering/sorting/selection, async commands/resources, validation,
errors, cancellation, and view lifetime. This is more valuable than more
abstract API surface because it validates ergonomics end-to-end.

Keep it small enough to maintain; it should be a representative app, not
a product.

---

## Next — useful, but not urgent

These are real framework improvements, but should be scheduled only after
`Now` is clean.

### Derived collections → UI adapter wiring

Make `FilteredList`, `SortedList`, and `MappedList` easy to consume from
existing list/table adapters so business code does not need a manual sync
layer.

Scope should stay adapter-level: no new collection model unless an
existing adapter cannot express the contract.

### JNI list source

Qt6, UIKit, and AppKit each ship a list/table source
(`ObservableListModel`, `ObservableTableSource`); JNI ships none. The
reviewer's workaround was to join list items with `"\n"` and split them
back in Kotlin, which discards item identity, per-row diffing, and
selection.

Add a JNI list source mirroring the existing adapter contract
(`RecyclerView.Adapter` on the Kotlin side, `ListChange` diffing on the
C++ side). Keep the scope identical to the other three — no new
collection model, no Compose-specific API.

Sequenced after the JNI documentation fix, because a list source nobody
can find repeats the current failure.

### Deterministic `Container` teardown

`Container` stores singletons in an `unordered_map` and destroys them via
`clear()`, so teardown order is unspecified. A service holding a
reference to another service can therefore observe a destroyed
dependency, and the failure is timing-dependent — the worst kind to
debug.

Record registration order and destroy in reverse (providers outlive
consumers). This is contained inside the existing `Container`
implementation and needs no new API.

Note: full dependency-graph-ordered teardown is a larger design and is
**not** in scope here — see *Evaluated and declined*.

### UIKit simulator CI

The UIKit conformance battery should eventually run automatically on an
iOS simulator. This is a quality gate, not a feature. Use it to prevent
adapter regressions; do not let it expand into a mobile app test
platform.

### Race-aware async trace

`with_timeout`, `when_any`, and `when_all` already have race arbitration.
Expose winner / loser-cancel / timeout-mode events through the existing
`TraceSink` so async debugging has the same observability as reactive and
binding flows.

Do **not** build a DevTools UI for this as part of the task; first make
the trace data complete and testable.

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

The existing plugin-property demo is enough for current validation. Do
not turn it into a plugin framework.

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
is also already addressed at the layer that matters: `plugin-property-demo`
shows `extern "C"` + `IProperty<T>` type erasure crossing a `dylib`
boundary, which is the ABI-safe primitive. Composing modules on top of
that primitive is application work.

Toolchain probing (`vswhere`, `xcode-select`, NDK paths) is likewise
per-project; Aria ships CMake targets, not a build front-end.

### Application bootstrap orchestrator

Requested: a `CoreBootstrap` (or equivalent) that encodes the correct
startup sequence — inject executors, construct the core, load modules,
register views.

Declined as designed, accepted as documentation. The underlying
constraint is real and is in `Now`: executors must be installed before
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
`bind_text_oneway` versus `bind_text`, and after the `Now` work a
`Computed` source will make two-way binding a compile error rather than a
convention. A parallel declaration layer would need macros or codegen,
duplicate information the call site already carries, and can drift out of
sync with it.

If the naming is the actual complaint, fix the naming and the reference
table (both in `Now`) — not the binding model.

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
three concrete gaps the study cited are addressed in `Now` by smaller
means — `BindingEngine::adopt` for view-scoped subscriptions, ordered
`Container` teardown for service ordering — without adding a competing
lifetime primitive.

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
