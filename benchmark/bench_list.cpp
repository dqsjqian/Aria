// bench_list — ObservableList<T> mutation hot paths.
//
// Targets the metrics that move when ObservableList itself changes:
// per-insert / per-remove / per-move cost, snapshot read cost, and
// item-changed dispatch with O(1) raw-pointer lookup.

#include "aria/aria.hpp"
#include "aria/abi/version.hpp"
#include "aria/observable_list.hpp"

#include "bench_common.hpp"

#include <memory>
#include <random>

using namespace aria;
using namespace aria_bench;

namespace {
struct Plain { int v; };
auto make(int v) { return std::make_shared<Plain>(Plain{v}); }
}  // namespace

int main() {
    banner(std::string("aria v") + abi::version_string + " — bench_list");

    // push_back
    {
        const int N = 100'000;
        ObservableList<Plain> list;
        double ns = measure_ns(N, [&](int i) { list.push_back(make(i)); });
        row("ObservableList::push_back (no observers)", ns, N);
    }

    // insert at random position over a 10k-item list
    {
        ObservableList<Plain> list;
        for (int i = 0; i < 10'000; ++i) list.push_back(make(i));
        std::mt19937 rng{1};
        std::uniform_int_distribution<int> pos{0, 10'000};
        const int N = 5'000;
        double ns = measure_ns(N, [&](int i) {
            list.insert(static_cast<std::size_t>(pos(rng)), make(i));
        });
        row("ObservableList::insert at random pos (n=10k)", ns, N);
    }

    // remove_at random in 10k-item list
    {
        ObservableList<Plain> list;
        for (int i = 0; i < 10'000; ++i) list.push_back(make(i));
        std::mt19937 rng{2};
        const int N = 5'000;
        double ns = measure_ns(N, [&](int) {
            std::uniform_int_distribution<int> pos{0, static_cast<int>(list.size()) - 1};
            list.remove_at(static_cast<std::size_t>(pos(rng)));
        });
        row("ObservableList::remove_at random pos (n~10k)", ns, N);
    }

    // move random
    {
        ObservableList<Plain> list;
        for (int i = 0; i < 10'000; ++i) list.push_back(make(i));
        std::mt19937 rng{3};
        std::uniform_int_distribution<int> pos{0, 9'999};
        const int N = 10'000;
        double ns = measure_ns(N, [&](int) {
            list.move(static_cast<std::size_t>(pos(rng)),
                      static_cast<std::size_t>(pos(rng)));
        });
        row("ObservableList::move random (n=10k)", ns, N);
    }

    // snapshot
    {
        ObservableList<Plain> list;
        for (int i = 0; i < 10'000; ++i) list.push_back(make(i));
        const int N = 1'000;
        double ns = measure_ns(N, [&](int) {
            auto snap = list.snapshot();
            (void)snap;
        });
        row("ObservableList::snapshot (n=10k)", ns, N);
    }

    // observe + push_back (single-listener fanout)
    {
        ObservableList<Plain> list;
        volatile long long sink = 0;
        auto sub = list.observe([&](const ListChange<Plain>& ch) {
            sink += ch.item ? ch.item->v : 0;
        });
        const int N = 100'000;
        double ns = measure_ns(N, [&](int i) { list.push_back(make(i)); });
        (void)sink;
        row("ObservableList::push_back + 1 observer", ns, N);
    }

    // -------------------------------------------------------------------
    //  Percentile snapshot — feeds nightly check-bench.sh thresholds.
    //  push_back is the canonical write hot path; tail spikes here
    //  usually mean an unexpected reallocation / observer fanout cost.
    // -------------------------------------------------------------------
    {
        ObservableList<Plain> list;
        // Pre-grow once so the percentile run does not include the
        // initial vector growth allocations.
        for (int i = 0; i < 1024; ++i) list.push_back(make(i));

        constexpr int kSamples      = 256;
        constexpr int kOpsPerSample = 200;
        auto stats = measure_percentiles(kSamples, kOpsPerSample,
            [&](int i) { list.push_back(make(i)); });
        row_pct("ObservableList::push_back (no observers)", stats);
    }

    std::cout << "\n=== done ===\n";
    return 0;
}
