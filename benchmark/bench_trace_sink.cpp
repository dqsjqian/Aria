// bench_trace_sink.cpp — measure the D-24 disabled-diagnostics fast path.
//
// With no sink, has_trace_sink() reads an atomic presence flag and gated
// publication avoids constructing the payload. With a sink, publication
// acquires an owning snapshot and dispatches the event. The bare gate,
// gated/ungated publication, and enabled sink are measured separately.
// Relative timings depend on the payload, compiler and machine.
//
// Threshold key: "publish_trace gated, no sink (D-24 fast path)".

#include "bench_common.hpp"

#include "aria/diagnostics.hpp"

#include <atomic>
#include <string>

using namespace aria_bench;

int main() {
    constexpr int SAMPLES = 1'000;
    constexpr int PER     = 1'000;   // 1M ops total per measurement

    banner("TraceSink: disabled fast path (D-24)");

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
