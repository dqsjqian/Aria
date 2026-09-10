# Aria Roadmap

> Framework version: `1.2.1`. This is a working priority list, not a release
> schedule. Changes marked **Unreleased** are implemented on this branch and
> are not part of the published release.

## Direction

Aria is a C++20 MVVM framework: reactive state, bindings, commands,
collections, validation, async primitives, adapter contracts, and diagnostics.
Its purpose is to share ViewModel code across UI hosts.

Keep core correctness and lifecycle contracts ahead of adapter expansion.
Prefer changes justified by a failing workflow, a regression, an adoption gap,
or a concrete consumer. AriaTools owns the flagship application; Aria owns
regressions for its public APIs, adapters, HTTP protocol, and browser SDK.

## Shipped baseline

These capabilities already exist and should not be scheduled again:

- Read-only reactive sources in one-way bindings; two-way bindings take
  `Property<T>`. See the [binding guide](guide/binding.md).
- `BindingEngine::adopt`, native adapter `view_for` helpers, and
  `ViewAdapterBase` for custom adapter implementations.
- Dispatcher/executor bridges, executor-injection diagnostics, and the
  first-contact documentation in the READMEs and adapter guides.
- Reverse-registration-order `Container` teardown (L-40), and race-aware
  async tracing (D-31.1).
- JNI list sources, derived-list support in native list/table adapters,
  and UIKit simulator conformance and table-source tests.
- `QComboBox` text two-way binding: initial text, ViewModel changes, and
  control changes already synchronize through `bind_text`.

Release history is in [CHANGELOG.md](../CHANGELOG.md). Earlier roadmap
rationale remains available in this file's Git history.

## Implemented on this branch — Unreleased

The correctness audit produced the following repairs and regression coverage.
They are local changes awaiting the release verification gate below.

- Binding input from worker threads follows the configured dispatcher policy
  for scalars, converted text, and commands. Queued callbacks are invalidated
  by view destruction, engine clear, and engine destruction.
- Channel delivery belongs to the selected receiver, preventing stolen values
  and false EOF. Capacity zero supports sender/receiver rendezvous.
- HTTP view replacement destroys the old view outside the registry lock and
  preserves the replacement's state and subscriptions.
- HTTP honors a fixed worker pool and caps SSE admission to retain a worker
  for ordinary requests. Initial SSE state includes values, visibility, and
  enabled state for all view kinds, with ordering against live updates.
- HTTP validates input shape, view identity, field kinds, and numeric ranges;
  errors use JSON. Heartbeat waiting is interruptible, start/stop is
  serialized, and a stopped server reports port zero.
- The SDK tracks disconnect/reconnect and pending connection attempts, rejects
  failed HTTP requests, and implements the protocol 2 integer representation:
  safe integers remain JS numbers; larger integers use decimal strings on the
  wire and BigInt in the SDK. Legacy exact numeric server inputs remain valid.
- The HTTP guide uses the shipped ESM SDK and actual endpoints. A compiled
  example, real HTTP/SSE tests, Node SDK tests, and a real `QComboBox` text
  regression belong to Aria's test suite.

**Release gate:** full host CTest with HTTP and native adapters, the document
API check, and focused sanitizer checks on changed concurrency paths. Record
actual results and any unavailable platform coverage before marking a release.
HTTP protocol 2 and worker-capacity behavior require migration notes in the
[changelog](../CHANGELOG.md#unreleased) and [HTTP guide](guide/adapters/http.md).

## Now — publish the generated API reference

The Doxygen target (`ARIA_BUILD_DOCS=ON`, `aria_docs`) exists. CI builds and
uploads an HTML artifact; a stable public reference is still unshipped.
Complete this after the correctness release gate:

1. Publish generated HTML at a stable URL with a repeatable update process.
2. Link that URL from `README.md`, `README.en.md`, and `docs/index.md`.
3. Verify that the reference covers the supported public API and that its
   links resolve from the published site.

Use the generated reference unless an actual reader need requires more.

## Next — QComboBox options and selection

Text two-way binding is shipped. Remaining work is driven by a consumer that
needs a changing option list or selection beyond its displayed text:

- Bind option-list insertions, removals, moves, and replacements.
- Define selected-index and stable item-ID semantics, including `itemData`
  mapping, duplicate labels, removed selections, and an empty selection.
- Add real-control tests for ViewModel-to-view and view-to-ViewModel updates,
  preserving selection identity through option-list changes.

Keep the adapter surface limited to those demonstrated needs.

## Triggered — require a concrete consumer or failure

### Swift / SwiftUI

Trigger: a real SwiftUI consumer. First test Swift/C++ interop with the
existing templates, coroutines, callbacks, and ABI-facing interfaces. Choose
between direct interop and Objective-C++ bridging from that evidence before
adding an adapter.

### WASM

Trigger: in-browser C++ computation for which the HTTP adapter is unsuitable,
such as local CAD, media processing, or ML. Start with one workload and a small
interop experiment. The HTTP adapter already serves a browser UI backed by a
separate C++ process.

### AppRuntime consolidation

Trigger: shared-service interference between independent app roots, parallel
in-process tests, or teardown isolation failures. Consolidate ownership of
existing dispatcher, logger, and event-bus services only as needed.
`Container` is already caller-owned; HTTP dispatch repairs use the existing
binding boundary. Acceptance is reproducible isolation and teardown tests.

### ObservableList slot identity

Trigger: one object must occupy multiple independent logical rows and the
current distinct-row-object workaround is unsuitable. Preserve the documented
and tested duplicate-`shared_ptr` behavior until a replacement contract and
identity-preserving diff tests exist.

### Cross-toolchain ABI expansion

Trigger: an actual binary adapter or plugin needs a boundary beyond the
existing `IProperty<T>` smoke. Extend the cross-dylib acceptance tests for that
compiler/platform combination before making broader ABI claims.

### Integer / enum two-way binding

Trigger: a control needs enum selection through an integer value and existing
bindings cannot express it. `bind_text_converted` requires
`Converter<SomeEnum, std::string>` and supports a text representation only;
`Converter<SomeEnum, int>` cannot be passed to that API. Consider a narrow
underlying-type scalar binder once a concrete consumer supplies its semantics
and round-trip tests.

### Write provenance

Trigger: `TraceSink` and `GraphInspector` cannot resolve a real report of
“who wrote this property.” Prototype call-site attribution against that case,
keep diagnostics gated, and measure that release builds pay no added cost.

## Out of scope

- Widget creation, layout, styling, and a heavy declarative UI DSL.
- Application module systems, plugin runtimes, scaffolding/build front ends,
  bootstrap orchestration, and host-specific ViewModel base classes.
- Built-in string catalogues, configuration, storage, persistence, RPC,
  distributed reactive graphs, and general messaging-policy enforcement.
- A parallel binding metadata/schema system or another lifetime `Scope` tree;
  use the existing bindings, subscriptions, ViewModel tree, and cancellation.
- Forced global `EventBus` use or dependency-graph DI teardown without a real
  limitation in the existing per-instance bus and ordered `Container`.
- Broad ABI refactors and reflection-driven auto-binding without proven
  consumer needs and production toolchain support.

## Maintenance rule

Check implementation and tests before adding an item. Keep one current status,
a concrete problem or trigger, and an executable acceptance condition. Move
released work to the changelog; use Git history for extended rationale. Update
this list when evidence changes, without turning conditional work into a plan.
