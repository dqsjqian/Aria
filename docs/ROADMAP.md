# Aria Roadmap

> Development version: `2.0.0` (unreleased). This is a working priority list, not a release
> schedule. Changes marked **Unreleased** are implemented on this branch and
> are not part of the published release.

## Direction

Aria is an MVVM framework supporting C++23, with C++20 as the minimum:
reactive state, bindings, commands,
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
They remain unreleased until the release verification gate below is complete.

- ABI 2 supplies one compiled graph, diagnostics registry, scheduler base and
  node-ID sequence across compatible dynamic libraries. Installed core-only
  consumers and cross-library dependency propagation have acceptance tests.
- Property/Computed lifetime and exception paths, signal cancellation,
  coroutine scope accounting, command destruction, collection event ownership,
  duplicate occurrences and derived-view replay have regression coverage.
- Stable-ID option selection, package consumption, build cache transitions,
  and host/device validation are implemented. Breaking contracts are listed
  in the [2.0 migration guide](migration-2.0.md).
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
[changelog](../CHANGELOG.md) and [HTTP guide](guide/adapters/http.md).

## Implemented — generated API reference publication

The Doxygen target (`ARIA_BUILD_DOCS=ON`, `aria_docs`) and Pages workflow
build and validate the reference, including public API coverage and local
links. GitHub Pages uses the Actions deployment source. Every push to `main`
builds the reference, checks the generated pages, and publishes at
[the canonical API reference](https://dqsjqian.github.io/Aria/), linked from
both READMEs and the documentation index. A deployment is successful only
when the workflow's build and deploy jobs both pass and the public site loads.

Use the generated reference unless an actual reader need requires more.

## Implemented — option selection and integer conversion

`ObservableListModel` supplies changing QComboBox options through the common
owning list event protocol. `bind_combo_box_selection` preserves selection by
stable item ID, supports duplicate display labels and empty selection, and
has real-control regressions for insert/remove/move/replace. The adapter also
supports integer index binding and `BindingEngine::bind_int_converted` for
non-contiguous enum values. See the [Qt guide](guide/adapters/qt6.md).

## Language baseline

Retain C++20 as the minimum and validate opt-in C++23 builds. The available
Apple and Android standard libraries do not provide a common C++23 feature
set that justifies raising the minimum. See the
[C++23 evaluation](cpp23-evaluation.md) for compiler/link probes and limits.

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
per-occurrence identity is needed beyond row indices. Repeated shared handles
now receive ItemChanged for every valid occurrence with frozen indices; they
no longer rely on a last-index sentinel. A new public slot-ID API remains
conditional on a concrete consumer that needs identity independent of both
object identity and position.

### Cross-toolchain ABI expansion

Trigger: an actual binary adapter or plugin needs a boundary beyond the
existing `IProperty` and cross-library reactive tests. Extend the cross-dylib acceptance tests for that
compiler/platform combination before making broader ABI claims.

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
