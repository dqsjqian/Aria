#pragma once

// Shared timing helpers for the bench_* binaries. Header-only so each
// bench target gets its own translation unit and we don't need a tiny
// bench_common library.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace aria_bench {

using clk = std::chrono::steady_clock;

#ifndef ARIA_BENCH_CONTROL_STRETCH_PERCENT
#define ARIA_BENCH_CONTROL_STRETCH_PERCENT 0
#endif
inline constexpr int control_stretch_percent = ARIA_BENCH_CONTROL_STRETCH_PERCENT;

inline void control_banner() {
    std::cout << "C ARIA_BENCH_CONTROL stretch_percent=" << control_stretch_percent << '\n';
}

// A separate measurement-control binary can add a known real wall-clock delay
// after each unchanged operation batch. This validates the detector, not a
// specific algorithm regression. Normal binaries compile this branch away.
inline auto sample_duration(clk::time_point start, clk::time_point end) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    if constexpr (control_stretch_percent > 0) {
        const auto target = ns + (ns * control_stretch_percent + 99) / 100;
        const auto deadline = start + std::chrono::nanoseconds{target};
        while (clk::now() < deadline) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - start).count();
    }
    return ns;
}


/// Accumulator that keeps a benchmarked expression from being optimised
/// away, without `volatile`.
///
/// The obvious spelling is `volatile long long sink; sink += x;`, and
/// that is what these benches used to do. C++20 deprecated compound
/// assignment on volatile-qualified types, so AppleClang rejects it
/// under `-Wdeprecated-volatile -Werror` — which meant the macOS CI job
/// could not build the bench targets at all.
///
/// `std::atomic` with `memory_order_relaxed` serves the same purpose
/// here: the compiler may not elide the store, so the producing
/// expression stays live, and relaxed ordering on a single-threaded
/// accumulator adds no fence. Only feed it inside the loop; read it
/// once afterwards.
class Sink {
public:
    void feed(long long v) noexcept {
        acc_.fetch_add(v, std::memory_order_relaxed);
    }

    [[nodiscard]] long long value() const noexcept {
        return acc_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<long long> acc_{0};
};

template<typename Fn>
inline double measure_ns(int iterations, Fn&& fn) {
    auto t0 = clk::now();
    for (int i = 0; i < iterations; ++i) fn(i);
    auto t1 = clk::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    return double(ns) / iterations;
}

inline void row(const std::string& name, double ns, int iters) {
    std::cout << "  " << std::left << std::setw(54) << name
              << std::right << std::setw(10) << std::fixed << std::setprecision(1)
              << ns << " ns/op  (" << iters << " iters)\n";
}

inline void banner(const std::string& title) {
    std::cout << "\n=== " << title << " ===\n";
    std::cout << "  (-O3 -DNDEBUG)\n\n";
    control_banner();
}

// ─────────────────────────────────────────────────────────────────────
//  Percentile-aware measurement.
//
//  measure_ns gives one mean over all operations. Percentiles below
//  expose variation between repeated sample averages; they do not
//  measure individual-operation tail latency. Nightly P99 ceilings
//  apply to these batch averages, with clock overhead amortized.
//
//  measure_percentiles records one timestamp pair per logical "batch"
//  of `ops_per_sample` operations and folds the measured ns/op into a
//  vector. The caller chooses `samples` — total work done is
//  `samples * ops_per_sample` operations, identical to what
//  measure_ns(samples * ops_per_sample, fn) would do.
//
//  We sample in BATCHES rather than per-op because individual op
//  latencies on x86/arm are often dominated by clock-source overhead
//  (~20–30 ns per now() call). Batching keeps the overhead amortized
//  but does not guarantee stable tail estimates (64 samples makes P99 the maximum).
// ─────────────────────────────────────────────────────────────────────

struct PercentileStats {
    double mean_ns = 0.0;
    double p50_ns  = 0.0;
    double p95_ns  = 0.0;
    double p99_ns  = 0.0;
    int    samples = 0;
    int    ops_per_sample = 0;
    std::vector<double> sample_means_ns;
};

template<typename Fn>
inline PercentileStats measure_percentiles_prepared(int samples, int ops_per_sample,
                                                   auto&& prepare, Fn&& fn) {
    std::vector<double> ns_per_op;
    ns_per_op.reserve(static_cast<std::size_t>(samples));

    long long total_ns = 0;
    int op_index = 0;

    for (int s = 0; s < samples; ++s) {
        prepare(s); // Fixture construction/reset is deliberately outside timing.
        auto t0 = clk::now();
        for (int j = 0; j < ops_per_sample; ++j) {
            fn(op_index++);
        }
        auto t1 = clk::now();
        auto ns = sample_duration(t0, t1);
        total_ns += ns;
        ns_per_op.push_back(double(ns) / double(ops_per_sample));
    }

    PercentileStats out;
    out.samples        = samples;
    out.ops_per_sample = ops_per_sample;
    out.mean_ns        = double(total_ns) / (double(samples) * double(ops_per_sample));

    if (!ns_per_op.empty()) {
        out.sample_means_ns = ns_per_op;
        std::sort(ns_per_op.begin(), ns_per_op.end());
        auto pick = [&](double q) {
            // Nearest rank: ceil(q * N), converted to a zero-based index.
            const auto rank = static_cast<std::size_t>(
                std::ceil(q * static_cast<double>(ns_per_op.size())));
            const auto idx = std::min(ns_per_op.size() - 1, rank - 1);
            return ns_per_op[idx];
        };
        out.p50_ns = pick(0.50);
        out.p95_ns = pick(0.95);
        out.p99_ns = pick(0.99);
    }

    return out;
}

template<typename Fn>
inline PercentileStats measure_percentiles(int samples, int ops_per_sample, Fn&& fn) {
    return measure_percentiles_prepared(samples, ops_per_sample, [](int) {}, fn);
}

// Print a percentile-rich row alongside the existing mean-only `row()`
// output. The leading "P  " marker makes the line trivially greppable
// from CI scripts (check-bench.sh keys off it).
inline void row_pct(const std::string& name, const PercentileStats& s,
                    const char* marker = "P ") {
    std::cout << marker << std::left << std::setw(52) << name
              << std::right << std::fixed << std::setprecision(6)
              << "mean=" << std::setw(8) << s.mean_ns << "ns  "
              << "p50="  << std::setw(8) << s.p50_ns  << "ns  "
              << "p95="  << std::setw(8) << s.p95_ns  << "ns  "
              << "p99="  << std::setw(8) << s.p99_ns  << "ns"
              << "  (" << s.samples << "x" << s.ops_per_sample << ")\n";
    { // Raw ordered batch means are retained for every acceptance metric.
        std::cout << "S {\"metric\":" << std::quoted(name) << ",\"batch_means_ns\":[";
        bool first = true;
        for (double value : s.sample_means_ns) {
            if (!first) { std::cout << ','; }
            std::cout << std::setprecision(6) << value;
            first = false;
        }
        std::cout << "]}\n";
    }
}

}  // namespace aria_bench
