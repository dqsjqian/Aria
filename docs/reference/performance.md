# Performance and complexity

Aria separates framework bookkeeping from the cost of application values,
callbacks and executors. `Property<std::string>` equality and copying are not
constant-time operations, and a `SortedList` backed by vectors cannot insert in
O(log N) merely because it uses binary search.

The bounds below describe the current implementation. They exclude lock
contention, allocator latency and application callback cost unless stated.
Use the release benchmarks to measure a workload on its actual target.

## Principles

| ID | Contract |
|---|---|
| PERF-1 | State value, callback, allocation and propagation costs separately. |
| PERF-2 | A derived list handles non-Reset source events with incremental events. Reset input and explicit whole-policy replacement may rebuild the view. Incremental delivery does not imply constant-time computation. |
| PERF-3 | Disabled tracing takes an atomic presence check; construct expensive diagnostic payloads only after checking `has_trace_sink()`. |
| PERF-4 | Equal property writes stop after value comparison and do not propagate. |
| PERF-5 | Benchmark thresholds sample specific operations; they do not establish bounds for every API or every host. Investigate failures before changing a threshold. |

## Reactive graph

Let D be the number of dependencies recorded by one evaluation, P the pending
nodes in one flush round, and E the edges actually traversed. User compute,
comparison, copy and destruction costs are additional.

| Operation | Framework cost |
|---|---|
| `Property<T>::get()` / `peek()` | Value copy; tracked `get()` also scans the current small dependency set |
| `get_ref()` / `peek_ref()` | No value copy; tracked reads still register a dependency |
| `Property<T>::set(v)`, equal | One value comparison |
| `Property<T>::set(v)`, changed | Value construction/swap, invalidation and any resulting flush |
| `Property<T>::on_changed(fn)` / `bind(fn)` | Constant-size node/edge registration; `bind` also invokes the initial callback |
| `Computed<T>` construction | One eager evaluation and dependency registration; no default construction of T |
| `Computed<T>::get()` | Cached value copy when clean; otherwise upstream pulls and evaluation |
| `Computed<T>` / `Effect` evaluation | O(D²) worst-case small-vector dependency deduplication, O(D) edge reconciliation, plus user work |
| `Graph::flush()` | O(P log P) ordering per round, edge traversal and required evaluations; reentrant writes may add rounds |
| `Subscription::release()` | Release the owned registration; reactive edge removal is O(D), and callback capture destruction is additional |

The graph reuses pending, traversal, dependency-read and edge storage after it
has reached the workload's high-water mark. Growth, new subscriptions, copied
values and user callbacks can still allocate. This is not a blanket promise
that every property write or every framework API is allocation-free.

Property and graph access belong to the graph owner thread. Cross-thread
updates add the cost of the selected dispatcher and its queue.

## Collections

N denotes source rows, V visible rows, K the range length and S subscribers.
Hash-map bounds are expected bounds; user hash/equality costs are additional.
List events retain shared item handles and Reset snapshots. Keeping events
alive therefore retains their payloads; it does not deep-copy item values.

| Operation | Cost before observer work |
|---|---|
| `ObservableList::push_back` / `pop_back` | Amortized O(1) storage bookkeeping; item subscription setup/teardown when needed |
| `insert_at(i)` / `remove_at(i)` | O(N − i) vector shifts |
| `insert_range(i, ...)` / `remove_range(i, K)` | O(N − i + K), followed by K incremental events |
| `remove_all(predicate)` | O(N) predicate/compaction work and one event per removed row |
| `replace_at(i)` | Expected O(1) bookkeeping and item subscription replacement |
| `move(from, to)` | O(abs(from − to)) rotation |
| `clear()` | O(N) ownership teardown and one Reset event |
| `at(i)` / `size()` / `snapshot()` | O(1) / O(1) / O(N) shared-handle copy |
| Child `ItemChanged` | O(N) scan to find every occurrence of the changed object; one event per occurrence |
| `reconcile(next, key)` | Expected O(N + K) for unchanged order and tail appends; O(N²) worst-case vector reordering; ambiguous duplicate keys use an explicit rebuild |
| Event delivery | O(S) callback visits per event, plus user work; queued reentrant/concurrent batches retain their payloads |

Derived lists maintain local row mirrors so an intermediate event can be
replayed even when the upstream has already committed a later state.

| View / operation | Cost and behavior |
|---|---|
| `FilteredList`, tail insertion | Amortized O(1) plus one predicate call; existing mappings remain untouched |
| `FilteredList`, other structural source change | O(N + V) worst-case mapping/row maintenance plus predicate evaluation; membership-preserving ItemChanged has constant bookkeeping |
| `FilteredList::set_predicate` | O(N) predicate evaluation and mapping rebuild, then an incremental membership diff |
| `SortedList`, ordinary change | O(N) order validation, index maintenance and vector shifts, plus O(log N) insertion comparisons |
| `SortedList`, several already-mutated keys | O(N log N + N × M) repair for M emitted moves; repeated handles and batched distinct objects are both supported |
| `SortedList::set_comparator` / Reset | O(N log N) sorting and a Reset snapshot |
| `MappedList`, tail insertion | Amortized O(1) plus mapper cost; middle insert/remove shifts O(N) entries |
| `DistinctList` / `GroupedList` | Hash-based key/group lookup; tail insertion has an expected constant bookkeeping path, while source-order mapping and representative changes may scan/shift O(N) entries |
| `PagedList`, tail insertion beyond an unchanged full page | Amortized O(1) source-mirror append; no page copies or diff |
| `PagedList`, other source structural change | Source-mirror vector update plus page-window diff; O(N + W²) worst case for window size W |
| `PagedList`, page change | O(W²) worst-case identity matching and vector moves; output is bounded by the old and new window sizes |

The order check in `SortedList` is deliberate: multiple items can already have
new comparison keys before their individual ItemChanged events arrive. Binary
search on that temporarily unordered sequence would be incorrect. A comparator
must be a stable strict weak ordering during each operation. Mutating an item
concurrently with comparison is outside the collection's synchronization
contract.

## Async, binding and diagnostics

| Operation | Relevant costs |
|---|---|
| `AsyncCommand::execute` | Argument/callable ownership, executor scheduling and user coroutine work; CancelPrevious also visits in-flight invocations |
| Command cancellation/destruction | O(I) cancellation-source visits for I registered invocations; cancellation callbacks can run synchronously |
| `when_all` / `when_any` | O(K) child setup for K tasks, plus child work and completion dispatch |
| `with_timeout` | One task wrapper and timer registration; timer/executor behavior is backend-dependent |
| `AsyncResource` reuse/deduplication | Key comparison and current-state checks; cold fetch adds scheduling and user fetcher work |
| Binding registration | Subscription and lifetime-state allocation plus initial conversion/rendering |
| Binding update | Value/converter work, then direct delivery or dispatcher queueing; mutable converter state is retained across updates |
| `has_trace_sink()` | O(1) atomic load |
| Gated trace publication, no sink | Atomic presence check; no diagnostic payload construction when gated at the call site |
| Trace publication with a sink | Shared callback snapshot under a mutex, then callback invocation outside the mutex |
| Sink replacement | Registry exchange and old capture destruction outside the lock |

`Loadable<T>`, `std::any` and validator result costs depend on their value,
message and collection sizes. Framework wrappers do not make those copies or
comparisons constant-time.

## 1.2.1 baseline versus 2.0 development

Measured on an Apple M3 Pro with Apple Clang 21.0.0, C++20 Release,
`-O3 -DNDEBUG`, using the same benchmark sources and compiler flags against
both complete production trees. The old revision is `eeb613f`; the measured
2.0 snapshot is `c33850d`, including the ABI 2 and collection fixes documented
in the changelog.
Each executable had one discarded warmup, followed by five alternating
paired runs. Values below are medians of the reported mean time per operation,
not best-of-five results. Timing includes the allocations performed by each
scenario; collection setup is outside its timed loop.

| Scenario | 1.2.1 | 2.0 development | Time change |
|---|---:|---:|---:|
| Property set, no observers | 20.5 ns | 13.2 ns | −36% |
| Property set, one observer | 98.9 ns | 35.0 ns | −65% |
| Property set, ten observers | 570.1 ns | 277.0 ns | −51% |
| Five-level Computed chain | 570.8 ns | 229.0 ns | −60% |
| Ten property sets in one batch | 250.6 ns | 102.5 ns | −59% |
| AsyncCommand round trip | 6.24 μs | 6.25 μs | Approximately unchanged |
| List append, no observers | 148.1 ns | 164.7 ns | +11% |
| List append, one observer | 137.2 ns | 193.1 ns | +41% |
| List random insert, about 10k rows | 55.45 μs | 33.18 μs | −40% |
| List random move, about 10k rows | 307.27 μs | 30.77 μs | −90% |
| FilteredList tail append, 10k initial rows | 11.18 μs | 0.23 μs | −98% |
| SortedList random-key append, 10k initial rows | 8.38 μs | 17.28 μs | +106% |
| SortedList random replacement, 10k rows | 7.11 μs | 19.62 μs | +176% |
| MappedList tail append, 10k initial rows | 189.3 ns | 212.2 ns | +12% |
| DistinctList new-key append, 100k initial rows | 1.90 μs | 2.50 μs | +32% |
| PagedList append beyond a full page, 100k initial rows | 159.4 ns | 439.5 ns | +176% |
| PagedList page hop, 50-row page | 4.54 μs | 4.20 μs | −7% |
| GroupedList append to an existing group, 100k initial rows | 757.0 ns | 616.1 ns | −19% |

The macOS ARM64 SortedList P99 budget was deliberately re-established at
130 μs after paired runs measured a median 85.6 μs with the new ordering
contract (37.9 μs in the old tree). This retains the heavy-operation 1.5×
margin. The FilteredList append budget was tightened from 41 μs to 1.5 μs.
The other ceilings are unchanged.

These are workload measurements, not an overall framework speedup. In
particular, generic SortedList validates live comparison keys before binary
search, including keys changed before their individual notifications. Removing
that check made batched updates incorrect. PagedList now retains a replayable
source mirror; its append scenario includes the mirror's first capacity growth
inside a 1,000-operation timed loop. The other ownership and cancellation
changes also add work to some short event paths.

Hash-table/allocator-heavy scenarios such as DistinctList varied substantially
between paired measurement sessions. Small deltas and this host's exact ratios
must not be extrapolated to Windows, Linux or mobile devices. Measure the
application's value types, comparator and update pattern on the target device.

## Reproducible measurements

Build with `CMAKE_BUILD_TYPE=Release` and `ARIA_BUILD_BENCHMARK=ON`, then run
`scripts/check-bench.sh build/flavors/release`. The six benchmark executables
also report their full scenario tables when run directly.

Percentile rows use a steady clock and nearest-rank P50/P95/P99 over repeated
samples. Each sample averages several operations; these percentiles describe
sample averages, not the latency distribution of individual operations.
`--runs N` reports the lowest P99 per metric across N complete runs. This can
reduce local scheduling noise, but must be disclosed when comparing results.
Use single-run results or retain every run when studying latency variability.

[thresholds.json](../../benchmark/thresholds.json) contains the measured metric
names and host-specific ceilings. The check rejects malformed, non-finite,
duplicate and missing measurements, including a missing metric in only one
of several runs. A new percentile metric must be registered in that file;
a new executable must also be registered in the benchmark CMake and scripts.

Compare the same workload, compiler, optimization flags and host at both
revisions. Run benchmarks after builds/tests finish, and report ownership or
memory costs alongside speedups. A threshold failure is evidence to investigate,
not a reason to silently loosen the gate.

See [lifecycle](lifecycle.md), [list events](list-diff-contract.md),
[diagnostics](diagnostics.md) and [error behavior](error-model.md).
