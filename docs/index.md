# Aria Documentation

A C++20 MVVM framework — one core, every platform.

---

## Getting Started

- **[Getting Started →](getting-started.md)** — build, first Property, Computed, Command, ObservableList, Validation, Async, and a real ViewModel in 5 minutes.

## Guides

Step-by-step, chapter-style documentation for library users:

| # | Chapter | Description |
|---|---------|-------------|
| 1 | [Reactive Core](guide/reactive-core.md) | `Property<T>`, `Computed<T>`, `Effect`, `Subscription`, `batch`, `untracked` |
| 2 | [ViewModel](guide/viewmodel.md) | `ViewModel` base class, lifecycle, child composition, `ViewModelScope`, destroy hooks, common patterns |
| 3 | [Async & Coroutines](guide/async.md) | `Task<T>`, `AsyncCommand`, `AsyncResource`, `with_timeout`, `when_any`, `when_all`, `CancellationToken` |
| 4 | [Collections](guide/collections.md) | `ObservableList<T>`, `FilteredList`, `SortedList`, `MappedList`, `DistinctList`, `PagedList`, `GroupedList`, `ListChange` |
| 5 | [Validation](guide/validation.md) | `Validator<T>`, `FormValidator`, `ValidationState`, `ValidationKey`, async rules |
| 6 | [View Binding](guide/binding.md) | `BindingEngine`, `IViewAdapter`, `IView`, `Converter`, two-way binding, feedback-loop suppression |
| 7 | [Navigation](guide/navigation.md) | `Navigator`, `push`/`pop`/`clear`, `push_for_result<R>`, `Modal` vs `Push`, deep-link routing |
| 8 | [Diagnostics & Debugging](guide/diagnostics-guide.md) | `TraceEvent`, `TraceSink`, `GraphInspector`, `ScopedTraceSink`, zero-overhead contract |
| 9 | [Adapters](guide/adapters/) | Platform-specific integration guides |

### Adapter Guides

| Adapter | Guide | Platform |
|---------|-------|----------|
| Qt6 | [qt6.md](guide/adapters/qt6.md) | Desktop (Qt 6.x) |
| AppKit | [appkit.md](guide/adapters/appkit.md) | macOS native |
| UIKit | [uikit.md](guide/adapters/uikit.md) | iOS native |
| JNI / Android | [jni.md](guide/adapters/jni.md) | Android (NDK + Compose) |
| HTTP / REST / SSE | [http.md](guide/adapters/http.md) | Browser (thin client) |

## Cookbook

Task-oriented recipes — short, self-contained, grounded in shipped APIs:

| # | Recipe |
|---|--------|
| — | [Cookbook index](cookbook/README.md) |
| 1 | [Form with sync + async rules](cookbook/01-form-sync-async-rules.md) |
| 2 | [Cross-field rule](cookbook/02-cross-field-rule.md) |
| 3 | [List: filter + sort + selection](cookbook/03-list-filter-sort-select.md) |
| 4 | [Pull-to-refresh + infinite scroll](cookbook/04-pull-refresh-infinite-scroll.md) |
| 5 | [Theme / Locale switching](cookbook/05-theme-locale-switching.md) |
| 6 | [`with_timeout` + `when_any` race](cookbook/06-timeout-when-any-race.md) |
| 7 | [View-destroy cancellation](cookbook/07-view-destroy-cancellation.md) |
| 8 | [Writing a new `IViewAdapter`](cookbook/08-writing-a-view-adapter.md) |

The full symbol-level **API reference** is Doxygen-generated:
`cmake -B build/flavors/docs -DARIA_BUILD_DOCS=ON && cmake --build build/flavors/docs --target aria_docs`
→ `build/docs/html/index.html`.

## Reference

Contract documents for framework developers and advanced users:

| Document | Description |
|----------|-------------|
| [API Style Contract](reference/api-style.md) | Naming, namespace, include-path, error-message, template-diagnostic, deprecation rules |
| [Lifecycle & Threading](reference/lifecycle.md) | Thread-affinity, subscription detach, view-destroy, coroutine race model |
| [Error Model](reference/error-model.md) | `ErrorKind` taxonomy, `aria::Error`, per-surface protocol, when to throw vs set |
| [Diagnostics Protocol](reference/diagnostics.md) | `TraceEvent`, `TraceSink`, per-subsystem hook points, zero-overhead contract |
| [List Diff Contract](reference/list-diff-contract.md) | `ListChangeKind`, event semantics, deterministic ordering, adapter conformance |
| [Performance Baselines](reference/performance.md) | Complexity bounds, measured baselines, anti-patterns |

## Architecture

- **[Architecture →](architecture.md)** — layer model, ABI stability, memory ownership, coroutine race model.

## RFCs

- [RFC 0001 — HTTP/REST/SSE Adapter](rfc/0001-http-adapter.md)

## Roadmap

- **[Roadmap →](ROADMAP.md)** — Now / Next / Triggered / Won't do.
