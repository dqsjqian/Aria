#pragma once

// Shared timing helpers for the bench_* binaries. Header-only so each
// bench target gets its own translation unit and we don't need a tiny
// bench_common library.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace aria_bench {

using clk = std::chrono::high_resolution_clock;

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
}

// ─────────────────────────────────────────────────────────────────────
//  Percentile-aware measurement.
//
//  measure_ns above gives us the *mean* over `iterations` ops, which
//  is great for tracking general drift but completely hides tail
//  latency. Nightly thresholds care about P99 — a regression in the
//  long tail (extra allocation, lock contention, scheduler hiccup)
//  rarely shows up in the mean until it has done substantial damage.
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
//  while still giving enough samples for stable P95/P99.
// ─────────────────────────────────────────────────────────────────────

struct PercentileStats {
    double mean_ns = 0.0;
    double p50_ns  = 0.0;
    double p95_ns  = 0.0;
    double p99_ns  = 0.0;
    int    samples = 0;
    int    ops_per_sample = 0;
};

template<typename Fn>
inline PercentileStats measure_percentiles(int samples, int ops_per_sample, Fn&& fn) {
    std::vector<double> ns_per_op;
    ns_per_op.reserve(static_cast<std::size_t>(samples));

    long long total_ns = 0;
    int op_index = 0;

    for (int s = 0; s < samples; ++s) {
        auto t0 = clk::now();
        for (int j = 0; j < ops_per_sample; ++j) {
            fn(op_index++);
        }
        auto t1 = clk::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        total_ns += ns;
        ns_per_op.push_back(double(ns) / double(ops_per_sample));
    }

    PercentileStats out;
    out.samples        = samples;
    out.ops_per_sample = ops_per_sample;
    out.mean_ns        = double(total_ns) / double(samples * ops_per_sample);

    if (!ns_per_op.empty()) {
        std::sort(ns_per_op.begin(), ns_per_op.end());
        auto pick = [&](double q) {
            // Nearest-rank percentile (pin to last element on overflow).
            const double scale =
                static_cast<double>(ns_per_op.size() - 1);
            std::size_t idx = static_cast<std::size_t>(q * scale);
            return ns_per_op[idx];
        };
        out.p50_ns = pick(0.50);
        out.p95_ns = pick(0.95);
        out.p99_ns = pick(0.99);
    }

    return out;
}

// Print a percentile-rich row alongside the existing mean-only `row()`
// output. The leading "P  " marker makes the line trivially greppable
// from CI scripts (check-bench.sh keys off it).
inline void row_pct(const std::string& name, const PercentileStats& s) {
    std::cout << "P " << std::left << std::setw(52) << name
              << std::right << std::fixed << std::setprecision(1)
              << "mean=" << std::setw(8) << s.mean_ns << "ns  "
              << "p50="  << std::setw(8) << s.p50_ns  << "ns  "
              << "p95="  << std::setw(8) << s.p95_ns  << "ns  "
              << "p99="  << std::setw(8) << s.p99_ns  << "ns"
              << "  (" << s.samples << "x" << s.ops_per_sample << ")\n";
}

}  // namespace aria_bench
