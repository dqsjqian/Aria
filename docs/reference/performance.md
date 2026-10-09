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
| PERF-2 | A derived list handles successful non-Reset source events with incremental events. Reset input, explicit whole-policy replacement and recovery after a failed user projection callback may rebuild the view. Incremental delivery does not imply constant-time computation. |
| PERF-3 | Disabled tracing takes an atomic presence check; construct expensive diagnostic payloads only after checking `has_trace_sink()`. |
| PERF-4 | Equal property writes stop after value comparison and do not propagate. |
| PERF-5 | Benchmark thresholds sample specific operations; they do not establish bounds for every API or every host. Investigate failures before changing a threshold. |

The nightly percentile gates measure distributions of **batch-average time per
operation**, not individual-operation tail latency. For example, the historical AsyncCommand `P` workload uses 64 batches of 50
executions, so P99 is the largest batch average. The separate fixed-window
`R` workload uses 1,024 batches to better resolve the upper tail. Run it on an otherwise idle host: competing builds
affect scheduling measurements. `check-bench.sh --runs N` reports the worst
P99 across N runs and retains every run; retries cannot erase a violation.

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

Sorted updates keep an O(N) reusable working layout and commit only successful
comparison results. This preserves the old projection if a comparator throws;
the layouts are synchronized incrementally after ordinary events. This adds
memory and bookkeeping without changing the existing O(N) ordinary-update
bound. Callback failure recovery may rebuild and emit Reset
as specified by D-29 in the [list diff contract](list-diff-contract.md).

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
both complete production trees. The old revision is `aa039f6`; the measured
2.0 snapshot is `2f22e17`, including the ABI 2 and collection fixes documented
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

## Collection hot-path maintenance (2026-10-08)

Against commit `5f95755`, the same Apple M-series host and AppleClang 21 Release
build measured five paired runs in alternating AB/BA order after warmup. The
median of each run's mean and sample-average P99 changed as follows:

| Scenario | Mean before / after (ns) | P99 before / after (ns) |
|---|---:|---:|
| ObservableList push_back, no observers | 139.5 / 110.1 | 526.7 / 494.0 |
| FilteredList source push_back | 216.4 / 189.4 | 603.3 / 575.4 |
| SortedList source push_back, random key | 48,483.5 / 41,083.7 | 84,262.1 / 70,409.0 |

The changes remove unused subscription bookkeeping for non-reactive element
types and reduce repeated indexing/branching in SortedList's full live-order
scan. They do not remove comparisons, transaction staging, stable tie-breaking
or reactive subscription installation. The benchmark workload and ceilings are
unchanged. These are developer-host measurements with normal background load,
not a claim that hosted-runner performance gates pass or all workloads improve.

## Further cost isolation

A bare worker-to-UI round-trip control now prints a diagnostic `D` row after
the existing AsyncCommand gate. It uses the same executors, pump and sample
shape without command properties or coroutine frames; it does not replace the
original gated workload or change any ceiling. Compare it with the command row
before attributing host scheduling latency to command bookkeeping.

Concrete SortedList callables now retain their type inside one shared target:
one erased call enters the complete scan, whose individual comparisons can be
inlined. The public erased Comparator overload remains supported. An ordered
source-tail insertion completes its comparisons and all required allocations
before committing a non-throwing index/handle update under the layout lock.
Other updates retain the existing transactional scratch path. Full live-key
order validation and stable equal-key ordering remain mandatory.

## Reproducible measurements and acceptance

Performance acceptance has two independent results. Neither is an industry
standard or a promise about individual-operation latency.

1. **Historical absolute budgets** in
   [thresholds.json](../../benchmark/thresholds.json) retain every original
   numeric ceiling and every original `P` workload. They describe batch-mean
   percentiles on a physical Apple M3 Pro with AppleClang 21, Release and no
   sanitizers. `scripts/check-bench.sh <build-dir>` records the machine,
   compiler, canonical CMake Release flags and 1/5/15-minute load before and after
   measurement. Custom flags, launchers or toolchains cannot claim calibration.
   The operational idle-profile check requires every load average to be at most
   half the logical CPU count; this validity check is not a new calibration.
   An incompatible or busy host returns **3, unavailable**, with all raw values
   and reference comparisons intact. A compatible host returns 1 on a budget
   violation, 0 only if every metric meets its ceiling. The OS/architecture
   label alone cannot qualify a hosted VM. `--runs N` now checks **all** runs
   and reports the worst batch-mean P99; it cannot select a passing retry.
2. **Same-host regression** uses a separate, versioned fixed-window workload
   and the full production trees of the candidate and published `v3.1.1`.
   Nightly copies the exact candidate benchmark source/header/CMake harness
   into the release checkout, then builds both with the same compiler and
   Release options. It records source revisions, harness and binary hashes,
   host details, warmups and every raw measurement. All `P` and `R` rows
   retain every batch mean in sample order at six decimal places; the parser
   independently reconstructs the mean and nearest-rank quantiles before
   accepting them. Rounding uncertainty can only widen the ratio interval. The baseline is deliberately
   pinned; publishing a new release does not silently move the comparison goal.

The six historical executables still expose their original `P` scenarios.
Their collection workloads grow through the timed run: ObservableList starts
at 1,024 rows and ends at 52,224; Filtered/Sorted start at 10,000 and end at
35,600. The historical `n=10k` label describes the initial size. Those results
must not be presented as fixed-size collection measurements.

`aria_bench_regression` adds `R` rows identified by `fixed-window-v2`:

| Scenario | Each timed batch | Batches per executable run |
|---|---|---:|
| ObservableList append | 1,024 through 1,223 pre-insert rows, 200 allocations/appends | 1,024 |
| FilteredList append | 10,000 through 10,199 pre-insert rows, 200 allocations/appends | 1,024 |
| SortedList random-key append | Same 10,000-row seed and random keys, 200 allocations/appends | 1,024 |
| AsyncCommand round trip | Two workers, 50 sequential execute/pump round trips | 1,024 |

Each collection fixture is recreated outside its timed batch, including the
same initial capacity-growth opportunity on both production trees. The workload
is a **fixed growth window**, not a claim that every operation has the same N.
Async includes allocation, dispatch, OS scheduling and return to the UI thread.
Every percentile uses `steady_clock`, batch elapsed time divided by operation
count, and nearest rank. At 1,024 batches, P99 is the 1,014th ordered batch
average. It is not the P99 of 51,200 individual commands. The earlier v1 pilot
retained the historical 64/128/256 batch counts and produced inconclusive tail
results; v2 increases precision without changing the 10% acceptance policy or
historical `P` measurements.

Build an additional baseline control tree with
`-DARIA_BENCH_CONTROL_STRETCH_PERCENT=20`. This adds a real, measured wall-clock
wait after each unchanged percentile batch, targeting 20% of its original
duration. The normal build compiles out this branch. The separate control
binary identifies itself in its output; mismatched identities fail validation.
Run the complete qualified gate with:

```sh
python3 scripts/run-bench-validation.py --baseline <release-bin-dir> \
  --candidate <candidate-bin-dir> --slow-control <delay-control-bin-dir> \
  --output <fresh-evidence-dir>
```

The runner freezes and hashes its analysis, qualification code and policy before
measurement and rechecks those files, executables and adjacent project shared
libraries after every phase. Library aliases are hashed through their targets;
adding, removing or replacing a library invalidates the measurement. Nonempty
`DYLD_*`, `LD_LIBRARY_PATH`, `LD_PRELOAD` or `LD_AUDIT` overrides are rejected
because they can bypass the recorded project library identities. System
libraries remain part of the recorded host/compiler environment.
It rejects any nonempty evidence directory without modifying the previous
attempt, including one interrupted before its first measurement. The three
independent phases are release versus itself (A-A), release versus the delay
control, then release versus candidate.

The fixed `paired-median-scenario-v3.1` design keeps **64 macro blocks** per
phase. Each macro contains one four-run block for each of the four fixed-window
(`R`) scenarios and eight blocks for each legacy (`P`) suite. Each `R` scenario
is launched separately with `aria_bench_regression --scenario list|filtered|sorted|async`.
A List pair no longer waits for Filtered, Sorted and Async measurements inside
its partner process. Each scenario retains its original fixture, reset, random
seed, 1,024 batches and 200/200/200/50 operations per batch. Async retains two
workers and its 100 warmup invocations. The default executable still runs all
four scenarios.

Every `R` metric has **64** paired observations and every `P` metric has
**512**. All twelve metrics retain **mean and batch-mean P99**, for 24 statistics.
The eight legacy metrics still cover Property/IProperty, observer, Computed
and four trace paths. There are 1,792 paired blocks and 7,168 formal process
calls per phase, plus 14 retained and excluded warmup processes. Scene selection
must match the exact requested metric; missing, extra or mislabeled rows fail.

The predeclared seed 20261009 shuffles the four scenario slots and eight legacy
group slots within each macro, and binary order within each legacy group.
Each scenario/suite has an independently shuffled balanced ABBA/BAAB orientation
list: 32 of each for `R`, 256 of each for `P`, with A the release and B the other
binary. The full schedule is saved before warmup or sampling. No sampling stops
early, extends after inspecting results or discards outliers. The checkpoint
retains scenario/suite, macro, paired-block and process sequence numbers, wall
and monotonic start/end timestamps, failed attempts and raw log references.

For each statistic, a block contributes the geometric ratio
`sqrt(B1 * B2 / (A1 * A2))`. The estimand remains the **population median of these
repeated-run ratios**, not the worst run, a ratio of pooled operations, or
individual-operation P99. The exact binomial order-statistic method described
by [NIST](https://www.itl.nist.gov/div898/software/dataplot/refman1/auxillar/mediancl.htm)
selects the 20th and 45th ordered ratios for `R64`, and the 221st and 292nd for
`P512`. Every statistic receives two-sided alpha at most `0.05 / 24`. The
resulting mixed-family joint coverage lower bound is **96.0681% per phase**,
conditional on independent, identically distributed block ratios within each
statistic. This is not a joint coverage claim for all three phases together.
Bonferroni does not require the different statistics to be independent.

The more densely spaced cheap blocks may cluster under a common temperature
or scheduling state. Increasing their count, interleaving cost families,
randomizing order and passing controls do not prove the iid assumption. These
assumptions and the timestamped observations must remain visible when reporting
results. Every block ratio, raw sample, repeat variation and worst-case
cross-pair envelope remains a diagnostic; none can silently replace the stated
estimand. Rounding can only widen the interval, and qualification independently
reconstructs each reported interval from the saved rounding bounds for every
block and checks its verdict.

[paired-policy.json](../../benchmark/paired-policy.json) retains the **1.10**
relative regression limit established before the earlier pilots. This 10%
bound is a project policy choice, not a historical budget or a physical or
statistical constant. An upper interval endpoint at most 1.10 passes; a lower
endpoint above 1.10 is a regression (exit 1); a crossing interval is
**inconclusive (exit 3)**. Exactly the predeclared 64/512 block counts are required.
The twelve **batch-mean** statistics carry the regression decision; the twelve
**batch-P99** statistics keep identical measurement and fixed-sample evidence
requirements but are recorded as **observations**, because the tail percentile
of batch means on shared hosts is dominated by scheduler noise rather than
library behavior.

All twelve A-A decision statistics must pass, and all twelve actual-delay
decision statistics must detect regression, before the candidate is measured.
If either control is inconclusive or contradicts its expected outcome on a
decision statistic, the result is **measurement-unavailable (exit 3)** and
candidate status is **not-measured**. This distinguishes an
unqualified measurement from candidate performance. An inconclusive batch-P99
interval is reported as an observation and does not disqualify the measurement.
The positive control tests sensitivity to an artificial 20% wall-clock delay;
it does not establish sensitivity to every 10% algorithm regression or
cross-platform performance.

Earlier four-scenario pilots used a maximum baseline-repeat veto; review found
that veto unnecessary for bounded paired improvement and insufficient to
establish independence. A subsequent ten-block full cross-pair-envelope pilot
left 17 of 24 A-A statistics inconclusive before rounding audit. The later
uniform 64-block median pilot qualified 23 of 24 A-A statistics; legacy
`IProperty::set_any` batch-mean P99 remained inconclusive. Its 256-batch P99 is
the third-largest batch mean per executable run. This motivates greater
predeclared replication across the entire cheap family while retaining every
historical sample shape. All earlier attempts keep their original results.
The stratified design requires entirely new A-A, real-delay and candidate
measurements; it never carries forward selected old passes or raises the 10%
budget. More observations improve precision under the model, but cannot
guarantee qualification on a future host or prove its assumptions.

Malformed/missing/duplicate/nonfinite/changed-workload data or failed binaries
return 2; partial bytes and completed measurements survive the failure.

Nightly requires the relative gate to pass. It separately records the absolute
budget result: hosted VMs remain explicitly uncalibrated, while a compatible
physical host must also pass the unchanged absolute gate. An absolute result
marked unavailable is never described as passing. A red relative gate cannot
be waived by the historical reference report or by a better mean alone.

Nightly runs the same complete protocol independently on three environments:
`macos-26` with AppleClang, `ubuntu-24.04` with GCC 14, and
`windows-2025-vs2026` with the actual VS2026 MSVC environment and Ninja Release.
All three performance jobs must qualify their own complete controls and pass
their own candidate measurements. A Linux or Windows pass cannot replace an
unavailable macOS measurement. TSan and fuzz remain separate macOS jobs.
Windows output explicitly identifies its MSVC `/O2` Release configuration;
project DLLs must be adjacent and are frozen alongside the `.exe` files.
The Windows PATH and working directory are recorded, while CRT, system and
side-by-side loader identities remain an explicit host-provenance boundary.

Runner labels and compiler versions do not fix the physical CPU or package
patch versions. Artifacts retain actual toolchain/build commands, CPU topology
and available platform load/memory/affinity/pressure information before and
after measurement. These observations do not adapt samples or acceptance.
Hosted VMs, Linux and Windows do not satisfy the historical physical M3 Pro
calibration; their absolute-budget results remain **unavailable**.

The earlier macOS run `37821972879` had two inconclusive A-A and two inconclusive
real-delay statistics and never measured the candidate. The v3 scenario
schedule addresses the long time between measurements of the same scenario;
it does not establish a particular scheduler cause or guarantee qualification.
Separating processes changes cache/allocator history, so all controls and
candidate samples must be collected afresh; no earlier pass is reused.
The single Linux-only v2 attempt is supplementary evidence, not closure of the
macOS failure. All 24 statistics, R64/P512 counts, original workloads and 1.10
limit remain intact. Concurrent runs queue rather than cancel active samples,
and a failing platform does not cancel another platform's campaign.

The separate physical M3 Pro campaign measured commit
`77098cf3b023a5cbe063e2e3af4e50198c6b8691` against published `v3.1.1`: all
24 A-A statistics passed, all 24 delay-control statistics detected the delay,
all 24 candidate statistics passed, and the 12 historical budgets passed in
one attempt. That result belongs to that measured commit and host. Later
workflow/documentation revisions are different source identities; they do not
inherit the physical measurement or establish Linux performance.

Run benchmarks after builds and tests finish. Keep every attempted measurement;
report ownership/memory costs with throughput. The bare-executor diagnostic `D`
row includes framework queueing and pumping, so it is neither a pure OS floor
nor something whose P99 can be subtracted from the command P99.

See [lifecycle](lifecycle.md), [list events](list-diff-contract.md),
[diagnostics](diagnostics.md) and [error behavior](error-model.md).
