// bench_iproperty.cpp — Compare type-erased IProperty access vs.
// templated Property<T> direct access.
//
// Goal: pin the cost numbers cited in `docs/architecture.md`'s
// "Coroutine race model" / IProperty section. The dominant overhead
// is expected to be the std::any copy on every get_any().

#include "bench_common.hpp"

#include "aria/i_property.hpp"
#include "aria/reactive/reactive.hpp"   // pulls graph.inl for Node defs

#include <any>

using namespace aria_bench;

int main() {
    constexpr int N = 1'000'000;

    banner("IProperty: type-erased vs. template-direct");

aria::Property<int> p{0};

    // ---------------------------------------------------------------
    //  GET path
    // ---------------------------------------------------------------

    // Baseline 1: peek() — no graph tracking, pure load.
    {
        volatile int sink = 0;
        auto ns = measure_ns(N, [&](int) {
            sink = p.peek();
        });
        row("Property<int>::peek()", ns, N);
    }

    // Baseline 2: get() with no active TrackingContext (still pays
    // the current_tracker() check + return).
    {
        volatile int sink = 0;
        auto ns = measure_ns(N, [&](int) {
            sink = p.get();
        });
        row("Property<int>::get()", ns, N);
    }

    // Type-erased: virtual call + std::any copy.
    {
        aria::IProperty& ip = p;
        volatile int sink = 0;
        auto ns = measure_ns(N, [&](int) {
            std::any a = ip.get_any();
            sink = std::any_cast<int>(a);
        });
        row("IProperty::get_any() + std::any_cast", ns, N);
    }

    // ---------------------------------------------------------------
    //  SET path
    // ---------------------------------------------------------------

    {
        auto ns = measure_ns(N, [&](int i) {
            p.set(i);   // equality-gated; alternates between two values
        });
        row("Property<int>::set(i)", ns, N);
    }

    {
        aria::IProperty& ip = p;
        auto ns = measure_ns(N, [&](int i) {
            (void)ip.set_any(std::any{i});
        });
        row("IProperty::set_any(std::any{i})", ns, N);
    }

    // ---------------------------------------------------------------
    //  Percentile snapshot — feeds nightly check-bench.sh thresholds.
    //  Mean numbers above are useful for humans; CI keys off P99 to
    //  catch tail regressions that the mean would smooth away.
    // ---------------------------------------------------------------
    {
        constexpr int kSamples       = 256;
        constexpr int kOpsPerSample  = 4'000;

        auto stats_set = measure_percentiles(kSamples, kOpsPerSample,
            [&](int i) { p.set(i); });
        row_pct("Property<int>::set(i)", stats_set);

        aria::IProperty& ip = p;
        auto stats_any = measure_percentiles(kSamples, kOpsPerSample,
            [&](int i) { (void)ip.set_any(std::any{i}); });
        row_pct("IProperty::set_any(std::any{i})", stats_any);
    }

    // ---------------------------------------------------------------
    //  Notes
    // ---------------------------------------------------------------
    //
    // Measured on Apple Silicon (M-series), -O3 -DNDEBUG.
    // bench_iproperty single-threaded run:
    //
    //   peek                  ~0.3 ns
    //   get                   ~1.4 ns   (current_tracker() check)
    //   get_any + any_cast    ~6.0 ns   (vcall + small-buffer any
    //                                     copy + any_cast template;
    //                                     the int payload fits in
    //                                     std::any's SBO so there is
    //                                     no heap traffic)
    //   set                   ~180 ns   (graph notify_changed pulse,
    //                                     dominated by reactive book-
    //                                     keeping; the equality gate
    //                                     keeps it cheap when alterna-
    //                                     ting values).
    //   set_any               ~187 ns   (~6 ns over set; the std::any
    //                                     copy is negligible against
    //                                     graph propagation cost).
    //
    // Takeaway: type-erasure adds ~5-7 ns over the template path on
    // get / set. For larger payload types the std::any copy will
    // grow (heap allocation kicks in past the SBO threshold), but
    // that is still the right tool for plug-in / RPC / live-binding
    // boundaries. For hot paths inside the host module, prefer
    // template-direct.

    return 0;
}
