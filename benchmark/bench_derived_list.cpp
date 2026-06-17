// bench_derived_list — FilteredList / SortedList / MappedList per-event cost.
//
// Each test runs an incremental scenario and records ns/op; results
// should always be sub-microsecond per source mutation, otherwise the
// derived view is failing the "incremental, not Reset" contract.
//
// As a sanity floor we also bench a "rebuild from scratch" baseline
// against std::vector — the derived-list cost should be in the same
// order of magnitude when the source is mostly static.

#include "aria/aria.hpp"
#include "aria/abi/version.hpp"
#include "aria/observable_list.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/sorted_list.hpp"
#include "aria/derived/mapped_list.hpp"
#include "aria/derived/distinct_list.hpp"
#include "aria/derived/paged_list.hpp"
#include "aria/derived/grouped_list.hpp"

#include "bench_common.hpp"

#include <algorithm>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

using namespace aria;
using namespace aria_bench;

namespace {
struct Item { int v; bool active; };
struct ItemView { int v; };
auto make(int v, bool a) { return std::make_shared<Item>(Item{v, a}); }
}  // namespace

int main() {
    banner(std::string("aria v") + abi::version_string + " — bench_derived_list");

    const int kSeed = 10'000;

    // FilteredList: per-insert into a 10k-seeded source
    {
        auto src = std::make_shared<ObservableList<Item>>();
        for (int i = 0; i < kSeed; ++i) src->push_back(make(i, i % 2 == 0));
        FilteredList<Item> filt(src, [](const Item& it){ return it.active; });

        const int N = 5'000;
        double ns = measure_ns(N, [&](int i) {
            src->push_back(make(kSeed + i, true));
        });
        row("FilteredList: source push_back (n=10k baseline)", ns, N);
    }

    // SortedList: per-insert at random key
    {
        auto src = std::make_shared<ObservableList<Item>>();
        for (int i = 0; i < kSeed; ++i) src->push_back(make(i, true));
        SortedList<Item> sorted(src,
            [](const Item& a, const Item& b) { return a.v < b.v; });

        std::mt19937 rng{42};
        std::uniform_int_distribution<int> dist{0, 1'000'000};
        const int N = 5'000;
        double ns = measure_ns(N, [&](int) {
            src->push_back(make(dist(rng), true));
        });
        row("SortedList: source push_back random key (n=10k)", ns, N);
    }

    // SortedList: ItemChanged crossing sort positions (uses Reactive
    // Property internally; we approximate with replace_at since Item
    // here is Plain — Replace with same-position triggers Replace, but
    // a sufficiently random replacement triggers Remove+Insert).
    {
        auto src = std::make_shared<ObservableList<Item>>();
        for (int i = 0; i < kSeed; ++i) src->push_back(make(i, true));
        SortedList<Item> sorted(src,
            [](const Item& a, const Item& b) { return a.v < b.v; });

        std::mt19937 rng{77};
        std::uniform_int_distribution<int> pos{0, kSeed - 1};
        std::uniform_int_distribution<int> val{0, 1'000'000};
        const int N = 5'000;
        double ns = measure_ns(N, [&](int) {
            src->replace_at(static_cast<std::size_t>(pos(rng)),
                            make(val(rng), true));
        });
        row("SortedList: source replace_at random (n=10k)", ns, N);
    }

    // MappedList: per-insert (mapper invocation cost dominates)
    {
        auto src = std::make_shared<ObservableList<Item>>();
        MappedList<Item, ItemView> mapped(src,
            [](const Item& it) { return std::make_shared<ItemView>(ItemView{it.v}); });
        for (int i = 0; i < kSeed; ++i) src->push_back(make(i, true));

        const int N = 5'000;
        double ns = measure_ns(N, [&](int i) {
            src->push_back(make(kSeed + i, true));
        });
        row("MappedList: source push_back (n=10k)", ns, N);
    }

    // Sanity floor: full rebuild of a sorted std::vector (no aria types)
    {
        std::vector<int> seed(kSeed);
        std::iota(seed.begin(), seed.end(), 0);
        const int N = 100;
        double ns = measure_ns(N, [&](int) {
            std::vector<int> copy = seed;
            std::sort(copy.begin(), copy.end());
        });
        row("std::sort full rebuild (n=10k, baseline)", ns, N);
    }

    // -------------------------------------------------------------------
    //  Percentile snapshot — feeds nightly check-bench.sh thresholds.
    //  Two derived-list incremental paths are pinned:
    //    1. FilteredList: source push_back  — predicate-only incremental.
    //    2. SortedList:   source push_back  — binary-search insert.
    //  Tail spikes usually mean a derived list fell back to a Reset.
    // -------------------------------------------------------------------
    {
        auto src = std::make_shared<ObservableList<Item>>();
        for (int i = 0; i < kSeed; ++i) src->push_back(make(i, i % 2 == 0));
        FilteredList<Item> filt(src, [](const Item& it){ return it.active; });

        constexpr int kSamples      = 128;
        constexpr int kOpsPerSample = 200;
        auto stats = measure_percentiles(kSamples, kOpsPerSample,
            [&](int i) { src->push_back(make(kSeed + i, true)); });
        row_pct("FilteredList: source push_back (n=10k)", stats);
    }
    {
        auto src = std::make_shared<ObservableList<Item>>();
        for (int i = 0; i < kSeed; ++i) src->push_back(make(i, true));
        SortedList<Item> sorted(src,
            [](const Item& a, const Item& b) { return a.v < b.v; });

        std::mt19937 rng{42};
        std::uniform_int_distribution<int> dist{0, 1'000'000};

        constexpr int kSamples      = 128;
        constexpr int kOpsPerSample = 200;
        auto stats = measure_percentiles(kSamples, kOpsPerSample,
            [&](int) { src->push_back(make(dist(rng), true)); });
        row_pct("SortedList: source push_back random (n=10k)", stats);
    }

    // -------------------------------------------------------------------
    //  >=10^5-element bench for the derived collections
    //  (DistinctList / PagedList / GroupedList). The contract being
    //  pinned: per-source-mutation cost stays sub-millisecond at 100k
    //  elements, i.e. derived collections never silently fall back to
    //  full Reset on a single source push_back.
    // -------------------------------------------------------------------
    constexpr int kBig = 100'000;

    // DistinctList: source push_back at n=100k. Every other element
    // is a duplicate to exercise the "hidden duplicate" branch.
    {
        auto src = std::make_shared<ObservableList<int>>();
        for (int i = 0; i < kBig; ++i) {
            src->push_back(std::make_shared<int>(i / 2));   // 0,0,1,1,2,2,...
        }
        DistinctList<int> dl{src};

        const int N = 1'000;
        double ns = measure_ns(N, [&](int i) {
            src->push_back(std::make_shared<int>(kBig + i));   // new key
        });
        row("DistinctList: source push_back new key (n=100k)", ns, N);
    }

    // PagedList: source push_back outside the current window at n=100k.
    // Outside-window inserts must be O(1) -- they don't slide the window.
    {
        auto src = std::make_shared<ObservableList<int>>();
        for (int i = 0; i < kBig; ++i) src->push_back(std::make_shared<int>(i));
        PagedList<int> pl{src, /*size=*/50, /*page=*/0};

        const int N = 1'000;
        double ns = measure_ns(N, [&](int i) {
            src->push_back(std::make_shared<int>(kBig + i));
        });
        row("PagedList: source push_back outside window (n=100k)", ns, N);
    }

    // PagedList: changing page_index across a 100k-element source must
    // re-window in time proportional to page_size, NOT to n.
    {
        auto src = std::make_shared<ObservableList<int>>();
        for (int i = 0; i < kBig; ++i) src->push_back(std::make_shared<int>(i));
        PagedList<int> pl{src, /*size=*/50, /*page=*/0};

        const int N = 200;
        std::size_t page = 0;
        double ns = measure_ns(N, [&](int) {
            page = (page + 1) % 100;
            pl.page_index().set(page);
        });
        row("PagedList: page_index hop (n=100k, page=50)", ns, N);
    }

    // GroupedList: source push_back at n=100k with 100 groups -- per-
    // insert cost is dominated by the inner ObservableList::push_back,
    // not by an outer rebuild.
    {
        auto src = std::make_shared<ObservableList<int>>();
        for (int i = 0; i < kBig; ++i) src->push_back(std::make_shared<int>(i % 100));
        GroupedList<int> g{src};

        const int N = 1'000;
        double ns = measure_ns(N, [&](int i) {
            src->push_back(std::make_shared<int>(i % 100));
        });
        row("GroupedList: source push_back into existing group (n=100k)", ns, N);
    }

    std::cout << "\n=== done ===\n";
    return 0;
}
