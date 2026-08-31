// bench_trace_sink.cpp — pin the D-24 zero-overhead contract for the
// diagnostic protocol.
//
// `docs/reference/diagnostics.md` D-24 states the no-sink fast path
// costs "one shared_ptr snapshot + null check at the call site; the
// inner block never runs", and §7 asks for a bench proving 1M
// publish_trace calls with no sink installed have negligible latency.
//
// A single absolute number cannot prove that: "12ns/op" means nothing
// without a reference. So this bench is built around COMPARISONS, each
// of which fails visibly if the contract breaks:
//
//   1. `has_trace_sink()` alone            — the gate's own cost.
//   2. gated publish, no sink              — the documented idiom
//                                            (AD2). Must be within
//                                            noise of (1), because the
//                                            payload is never built.
//   3. ungated publish_trace, no sink      — pays the payload
//                                            construction (two
//                                            std::strings) even though
//                                            nobody listens. This is
//                                            the AD2 anti-pattern, and
//                                            it should be visibly more
//                                            expensive than (2). If it
//                                            is NOT, the gating idiom
//                                            has stopped mattering and
//                                            the docs are misleading.
//   4. gated publish, sink installed       — the slow path, for scale.
//
// The ratio printed at the end is the actual assertion a reader cares
// about: how much does an installed sink cost relative to no sink.
//
// Threshold key: "publish_trace gated, no sink (D-24 fast path)" is the
// one wired into benchmark/thresholds.json — it is the number the
// framework actually promises.

#include "bench_common.hpp"

#include "aria/diagnostics.hpp"

#include <atomic>
#include <string>

using namespace aria_bench;

int main() {
    constexpr int SAMPLES = 1'000;
    constexpr int PER     = 1'000;   // 1M ops total per measurement

    banner("TraceSink: zero-overhead contract (D-24)");

    // Keep the compiler from proving the whole loop dead.
    volatile bool gate_sink = false;
    std::atomic<std::uint64_t> delivered{0};

    // ---------------------------------------------------------------
    //  1. The gate itself.
    // ---------------------------------------------------------------
    aria::clear_trace_sink();
    {
        auto s = measure_percentiles(SAMPLES, PER, [&](int) {
            gate_sink = aria::has_trace_sink();
        });
        row_pct("has_trace_sink() only (no sink)", s);
    }

    // ---------------------------------------------------------------
    //  2. The documented idiom: gate, then publish. No sink installed,
    //     so the payload is never constructed. This is the number
    //     D-24 is about.
    // ---------------------------------------------------------------
    {
        auto s = measure_percentiles(SAMPLES, PER, [&](int i) {
            if (aria::has_trace_sink()) {
                aria::publish_trace_unchecked(
                    aria::TraceCategory::List,
                    aria::trace::List{"Insert",
                                      static_cast<std::size_t>(i),
                                      0,
                                      static_cast<std::size_t>(i) + 1});
            }
        });
        row_pct("publish_trace gated, no sink (D-24 fast path)", s);
    }

    // ---------------------------------------------------------------
    //  3. AD2 anti-pattern: build the payload before checking. Same
    //     observable behaviour, strictly more work — the delta is
    //     exactly what the gating convention buys.
    // ---------------------------------------------------------------
    {
        auto s = measure_percentiles(SAMPLES, PER, [&](int i) {
            aria::publish_trace(
                aria::TraceCategory::List,
                aria::trace::List{"Insert",
                                  static_cast<std::size_t>(i),
                                  0,
                                  static_cast<std::size_t>(i) + 1});
        });
        row_pct("publish_trace ungated, no sink (AD2 anti-pattern)", s);
    }

    // ---------------------------------------------------------------
    //  4. Slow path, for scale: a sink is installed and does the
    //     cheapest useful thing (a relaxed counter bump).
    // ---------------------------------------------------------------
    {
        aria::ScopedTraceSink guard{[&delivered](const aria::TraceEvent&) {
            delivered.fetch_add(1, std::memory_order_relaxed);
        }};
        auto s = measure_percentiles(SAMPLES, PER, [&](int i) {
            if (aria::has_trace_sink()) {
                aria::publish_trace_unchecked(
                    aria::TraceCategory::List,
                    aria::trace::List{"Insert",
                                      static_cast<std::size_t>(i),
                                      0,
                                      static_cast<std::size_t>(i) + 1});
            }
        });
        row_pct("publish_trace gated, sink installed (slow path)", s);
    }

    // Guard against a vacuous run: if the sink never fired, case 4
    // measured nothing and the comparison above is meaningless.
    std::cout << "\n  sink deliveries during slow-path measurement: "
              << delivered.load() << "\n";
    if (delivered.load() == 0) {
        std::cout << "  !! slow path never delivered — bench is vacuous\n";
        return 1;
    }

    std::cout << "\n  D-24 reading guide:\n"
                 "    fast path should sit within noise of the bare gate;\n"
                 "    the ungated row shows what AD2 costs (payload built\n"
                 "    for nobody); the slow path is the price of an\n"
                 "    installed sink.\n";
    (void)gate_sink;
    return 0;
}
