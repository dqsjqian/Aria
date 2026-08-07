# Aria Roadmap

> Single source of truth for what is still wanted, deliberately deferred,
> or explicitly out of scope. The framework version stays `1.0.0`; this
> is a prioritised working list, not a release plan.

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

### Documentation state pass

Keep shipped/planned status consistent across:

- `README.md` / `README.zh-CN.md` and their tracked HTML mirrors;
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
