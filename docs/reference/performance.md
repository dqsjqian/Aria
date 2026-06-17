# Performance Baselines and Complexity Contracts

> Aria framework performance baselines and complexity contracts. Together
> with [`lifecycle.md`](./lifecycle.md), [`api-style.md`](./api-style.md),
> [`error-model.md`](./error-model.md), [`diagnostics.md`](./diagnostics.md)
> and [`list-diff-contract.md`](./list-diff-contract.md), this document
> forms the framework's six-pillar contract document family.

> One-liner: every public API has a complexity bound pinned once and for
> all; no subsequent change is allowed to regress that complexity. Doing
> so is a contract break.

A world-class framework needs an explicit non-functional contract: users
must know whether `Property::set` is O(1) or O(N), whether
`Computed::recompute` is on-demand or whole-graph, whether
`SortedList::source.push_back` is O(log N) or O(N). This document lists
the complexity bound of every **public operation** by subsystem, and
pins a set of **measured baselines** as the regression gate for the
nightly bench.

> **Important**: this document records contracts (complexity bounds),
> not specific micro-benchmark numbers. The baseline numbers are a
> reference; hardware / compiler / memory pressure jitter within ±2× is
> expected. Anything beyond that range means the implementation has
> drifted and must be investigated.

---

## Overall principles

| ID  | Principle |
| --- | ----- |
| **PERF-1** | Every public API's complexity bound must be writable as a row in a table. If the bound depends on an external callback (e.g. an `Effect`'s `fn`), that external cost must be split out and stated explicitly. |
| **PERF-2** | Derived collections (FilteredList / SortedList / MappedList / DistinctList / PagedList / GroupedList) **never** fall back to a full Reset on a single source event. Their complexity bound may climb to O(N_visible) (mid-range insert / mid-range delete paths), but the number of derived events emitted is ≤ O(items locally affected by the source change). |
| **PERF-3** | When **no sink / no observer** is attached, optional diagnostics and optional logging degrade to a single `shared_ptr` load + null check. |
| **PERF-4** | An "equal-value" reactive write (`equality-gated set`) must decide and short-circuit in O(1), with zero downstream propagation. |
| **PERF-5** | Every contract is sampled by `aria_bench_*` in nightly. Beyond a 2× window from this document is a regression. |

---

## Reactive Core

| Operation | Complexity bound | Notes |
|---|---|---|
| `Property<T>::Property(initial)` | O(1) | |
| `Property<T>::get()` / `peek()` | O(1) | |
| `Property<T>::set(v)` (equal value) | O(1) | PERF-4: equality-gated short-circuit |
| `Property<T>::set(v)` (real change) | O(D) | D = nodes directly subscribed to this property; total propagation is O(dirty-subgraph) |
| `Property<T>::on_changed(fn)` | O(1) register + O(emit) per fire | |
| `Property<T>::bind(fn)` | O(1) register + immediate `fn(initial)` | L-13 first-fire is the documented contract |
| `Computed<T>::Computed(fn)` | O(1) ctor + 1 immediate recompute | dependency edges auto-collected |
| `Computed<T>::recompute()` | O(deps) + O(fn) | dependency-edge re-collection |
| `Computed<T>::get()` | O(1) cached / O(deps) + O(fn) when dirty | |
| `Effect::Effect(fn)` | O(1) ctor + 1 immediate `fn` | |
| `Effect` re-trigger | O(deps) + O(fn) | |
| `Subscription::detach()` | O(1) | |
| `Graph::flush()` | O(dirty-subgraph) | mark-pull algorithm; clean branches are skipped |

**Property::set equal-value fast-path baseline** (reference: `aria_bench_property` on Apple M-class, -O3):

| Scenario | ns/op | Meaning |
|---|---|---|
| `Property<int>::set` same value | < 30 ns | equality gate hit |
| `Property<int>::set` real change + 1 `on_changed` | < 200 ns | propagation + sink |
| `Computed<int>` single-dep invalidate→recompute | < 300 ns | |

---

## ObservableList

| Operation | Complexity bound | Notes |
|---|---|---|
| `push_back` / `pop_back` | O(1) amortised + O(observers) emit | |
| `insert_at(i)` | O(N - i) + O(observers) | mid-range insert shifts the tail |
| `remove_at(i)` | O(N - i) + O(observers) | as above |
| `replace_at(i)` | O(1) + O(observers) | |
| `move(from, to)` | O(\|from-to\|) + O(observers) | rotate |
| `clear()` | O(N drop) + 1 Reset emit | |
| `at(i)` / `size()` / `snapshot()` | O(1) / O(1) / O(N copy) | snapshot is a strong-ref copy |
| `observe(fn)` | O(1) register | |
| Single ItemChanged forward | O(observers) | child T's on_changed → list ItemChanged |

> `snapshot()` is an O(N) copy — do not call it on hot paths; prefer
> `at(i)` or incremental subscription via `observe(fn)`. Derived
> collections' mid-range insert paths (DistinctList mid-insert /
> GroupedList mid-insert) compute a `derived_pos` once at O(N_source);
> tail push_back uses the fast path.

---

## Derived collections (core contract: per source event, never degrade to full Reset)

> **Important**: derived collections' complexity bounds are **honest
> upper bounds**, not marketing claims. Outside the tail-push_back
> fast path, mid-range insert / delete trigger O(N_visible)-class
> local rearrangements. The bench is not the contract; it's a sample
> of the contract under typical loads.

| Operation | Complexity bound | Measured baseline (typical load) |
|---|---|---|
| `FilteredList`: source push_back | O(1) amortised + 1 emit | ~12 µs/op (n=10k) |
| `FilteredList`: source ItemChanged crosses filter boundary | O(N_visible) | (workload-dependent) |
| `SortedList`: source push_back | O(log N) binary insert + O(M) shift | ~10 µs/op (n=10k) |
| `SortedList`: source ItemChanged crosses sort position | O(log N) + O(M) shift | (workload-dependent) |
| `MappedList`: source push_back | O(mapper) + 1 emit | ~480 ns/op (n=10k) |
| `DistinctList`: source push_back tail (new key) | O(1) amortised + 1 emit | ~3.8 µs/op (n=10⁵) |
| `DistinctList`: source push_back duplicate key | O(bag) amortised + 0 emits | (silent; bag is typically tiny) |
| `DistinctList`: source insert mid-range, new key | O(N_source) compute derived_pos + O(N_visible) shift + 1 emit | (only PD-2 source-order path) |
| `DistinctList`: source remove current representative | O(N_visible) erase + 1 Replace or Remove emit | (Replace path only when a hidden duplicate can be promoted) |
| `DistinctList`: ItemChanged that mutates the key | O(N_visible) | (resolves to the Remove + Insert pair above) |
| `PagedList`: source push_back outside the window | O(1) + 0 emits | ~370 ns/op (n=10⁵) |
| `PagedList`: source push_back inside the window | O(page_size) + 1~2 emits | |
| `PagedList`: page_index hop | O(page_size) diff + emit | ~7.5 µs/op (n=10⁵, page=50) |
| `GroupedList`: source push_back into existing group | O(1) + 0 outer emits | ~870 ns/op (n=10⁵) |
| `GroupedList`: source push_back tail, new group | O(1) + 1 outer Insert | |
| `GroupedList`: source insert mid-range, new group | O(N_source) compute outer_pos + O(N_groups) shift | |
| `GroupedList`: source remove last item in a group | O(N_g) inner erase + O(N_groups) shift + 1 outer Remove | |

**Contract (PERF-2 hard rule)**: derived collections **never** fall
back to a full Reset on a single source event. If the nightly bench
shows millisecond-level regressions on derived collections at the
100k scale, that means a regression happened — investigate
immediately.

---

## Validator / FormValidator

| Operation | Complexity bound | Notes |
|---|---|---|
| `Validator::rule(...)` | O(1) register + 1 `run_` | |
| `Validator::run_(v)` | O(rules) + O(async_errors) | fires on every source set |
| `Validator::begin_pending()` / `end_pending(...)` | O(1) state set + 1 `run_` | |
| `FormValidator::add(...)` | O(1) register + state aggregate | |
| `FormValidator::state()` recompute | O(fields) | |
| `AsyncValidator::attach_to(v, source)` | O(1) | |
| `AsyncValidator` single fire | O(1) schedule + factory cost | V-1 latest-wins; older fires are cancelled |
| `AsyncValidator` V-5 same-value dedupe | O(1) | |

---

## AsyncCommand / AsyncResource / Task

| Operation | Complexity bound | Notes |
|---|---|---|
| `AsyncCommand::execute(args)` | O(1) schedule + user coroutine cost | |
| `AsyncCommand::can_execute` propagation | O(observers) | via the reactive graph |
| `AsyncResource::fetch(key)` cache hit | O(1) | R-1 equivalent-key detection |
| `AsyncResource::fetch(key)` dedupe | O(1) | same key in-flight is collapsed |
| `AsyncResource::fetch(key)` cold | O(1) schedule + fetcher cost + 1 sync `is_loading` set | "synchronous takeoff" — caller sees Loading immediately |
| `AsyncResource::loadable` derived write | O(1) per state mutation | recompute_loadable_ writes at 5 transition points; equality-gated |
| `Task<T>::start_detached` | O(1) | |
| `with_timeout(task, deadline)` | O(1) wrap + 1 internal timer | |
| `when_any(tasks...)` | O(N tasks) wrap | each task runs on its own |
| `when_all(tasks...)` | O(N tasks) wrap | |

---

## Diagnostics

| Operation | Complexity bound | Notes |
|---|---|---|
| `has_trace_sink()` | O(1) atomic load | D-22 |
| `publish_trace(...)` no sink | 1 atomic load + 1 null check | PERF-3 |
| `publish_trace(...)` with sink | 1 shared_ptr snapshot + sink call | D-22 |
| `publish_trace_unchecked(...)` with sink | 1 shared_ptr snapshot + sink call | caller is responsible for has_trace_sink() gating |
| `install_trace_sink(fn)` | O(1) | write-lock serialised |
| `clear_trace_sink()` | O(1) | |
| `ScopedTraceSink` ctor / dtor | O(1) | |

---

## Loadable<T>

| Operation | Complexity bound |
|---|---|
| `Loadable<T>::idle / loading / refreshing / success / error` factories | O(1) + 1 `T` copy |
| `Loadable<T>::is_*` predicates | O(1) |
| `Loadable<T>::value_or(fb)` | O(1) + 1 `T` copy |
| `Loadable<T>::map(fn)` | O(1) + 1 `fn(T)` |
| `Property<Loadable<T>>::set` equal value | O(1) equality-gated |

---

## Binding

| Operation | Complexity bound | Notes |
|---|---|---|
| `BindingEngine::two_way(...)` | O(1) register + 1 immediate push | feedback-loop suppression state machine |
| Two-way binding single push | O(observers) | |
| `FormPanel::bind(...)` | O(1) per field | |

---

## ABI / IProperty

| Operation | Complexity bound |
|---|---|
| `IProperty<T>::get_any` / `set_any` | O(1) + 1 `std::any` copy |
| `subscribe_any(fn)` | O(1) register |

---

## Measured baseline snapshot (nightly check-bench.sh anchors)

The numbers below are a snapshot of the current commit (Apple M-class,
-O3 -DNDEBUG). Any commit causing > 2× regression on any row is a
contract break. These are PERF-2 / PERF-5 sampling points, **not the
complexity bound itself**.

| Subsystem | Operation | Baseline ns/op | Notes |
|---|---|---|---|
| ObservableList | push_back baseline (n=10k) | 200~500 ns | |
| FilteredList | source push_back (n=10k) | ~12 µs | |
| SortedList | source push_back random (n=10k) | ~10 µs | |
| MappedList | source push_back (n=10k) | ~480 ns | |
| DistinctList | source push_back tail new key (n=10⁵) | ~3.8 µs | PD-2 source-order cost |
| PagedList | source push_back outside window (n=10⁵) | ~370 ns | |
| PagedList | page_index hop (n=10⁵, page=50) | ~7.5 µs | |
| GroupedList | source push_back existing group (n=10⁵) | ~870 ns | |

> The numbers above only serve as regression-detection thresholds. The
> binding contracts above (PERF-1/PERF-2) are authoritative.

---

## Anti-pattern quick reference (don't write this)

| Anti-pattern | Consequence | Fix |
|---|---|---|
| Calling `source.snapshot()` from a derived list per event | Every event copies O(N) → millisecond stalls at n=10⁵ | Use `source.at(i)` or maintain a reverse mapping |
| Derived list reacts to a source event by `clear()` + rebuild | Always emits Reset; breaks PERF-2 | Incremental algorithm + precise Replace/Insert/Remove emits |
| `Property::set` without an equality gate | Every set propagates → whole-graph refresh storm | Use the framework's `set` — don't roll your own `notify` |
| Diagnostic path that builds a heavy payload before checking `has_trace_sink()` | Pay the cost even when nobody's listening | `if (has_trace_sink()) { ... publish_trace_unchecked(...); }` |
| Side effects inside a `Computed` body | Side effects fire spuriously on every recompute | Move side effects into `Effect` |

---

## Cross-document references

- [`lifecycle.md`](./lifecycle.md) **L-13 / L-21**: equality-gate +
  emit-snapshot — PERF-3 / PERF-4 here directly depend on those.
- [`list-diff-contract.md`](./list-diff-contract.md) **LD-2 / LD-7**:
  the incremental-event protocol — PERF-2 here is its performance side.
- [`diagnostics.md`](./diagnostics.md) **D-22**: sink fetch + zero-overhead
  path — PERF-3 here is the budget that backs it.
- [`error-model.md`](./error-model.md) **E-11**: equal errors are not
  re-fired — PERF-4 applied to the error stream.
