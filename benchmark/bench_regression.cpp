// Fixed-window v2 workload for same-host complete-library comparisons.
// R rows are batch-mean percentiles, never individual-operation quantiles.
// Historical P workloads remain unchanged in their original executables.
#include "aria/aria.hpp"
#include "aria/async/async_command.hpp"
#include "aria/derived/filtered_list.hpp"
#include "aria/derived/sorted_list.hpp"
#include "bench_common.hpp"

#include <memory>
#include <random>
#include <stdexcept>

namespace {
struct Item { int v; bool active; };
auto make(int value, bool active = true) {
    return std::make_shared<Item>(Item{.v = value, .active = active});
}
using Source = aria::ObservableList<Item>;
constexpr int samples = 1024;
constexpr int operations = 200;
constexpr int seed_size = 10'000;
auto seeded(int size) {
    auto source = std::make_shared<Source>();
    for (int i = 0; i < size; ++i) { source->push_back(make(i, i % 2 == 0)); }
    return source;
}
} // namespace

int main() try {
    using namespace aria_bench;
    control_banner();
    std::cout << "ARIA_WORKLOAD fixed-window-v2 batch-mean-nearest-rank\n";
    {
        std::shared_ptr<Source> source;
        auto stats = measure_percentiles_prepared(samples, operations,
            [&](int) { source = seeded(1024); },
            [&](int i) { source->push_back(make(1024 + (i % operations))); });
        row_pct("List append [1024,1224)", stats, "R ");
    }
    {
        std::shared_ptr<Source> source;
        std::unique_ptr<aria::FilteredList<Item>> view;
        auto stats = measure_percentiles_prepared(samples, operations,
            [&](int) {
                view.reset();
                source = seeded(seed_size);
                view = std::make_unique<aria::FilteredList<Item>>(
                    source, [](const Item& item) { return item.active; });
            },
            [&](int i) { source->push_back(make(seed_size + (i % operations))); });
        row_pct("Filtered append [10000,10200)", stats, "R ");
    }
    {
        std::shared_ptr<Source> source;
        std::unique_ptr<aria::SortedList<Item>> view;
        std::mt19937 random{42}; // NOLINT(bugprone-random-generator-seed): reproducible benchmark keys.
        std::uniform_int_distribution<int> keys{0, 1'000'000};
        auto stats = measure_percentiles_prepared(samples, operations,
            [&](int) {
                view.reset();
                source = seeded(seed_size);
                view = std::make_unique<aria::SortedList<Item>>(
                    source, [](const Item& a, const Item& b) { return a.v < b.v; });
                random.seed(42); // NOLINT(bugprone-random-generator-seed): same keys in every timed window.
            },
            [&](int) { source->push_back(make(keys(random))); });
        row_pct("Sorted random append [10000,10200)", stats, "R ");
    }
    {
        aria::async::MainThreadExecutor ui;
        aria::async::ThreadPoolExecutor worker(2);
        aria::async::AsyncCommand<int, int> command(ui, worker,
            [](int value) -> aria::async::Task<int> { co_return value * 2; });
        auto invoke = [&](int value) {
            command.execute(value);
            if (!ui.pump_until([&] { return !command.is_executing.get(); },
                              std::chrono::milliseconds{500})) {
                throw std::runtime_error("AsyncCommand fixed workload timed out");
            }
        };
        for (int i = 0; i < 100; ++i) { invoke(i); }
        row_pct("AsyncCommand round-trip [2 workers]",
                measure_percentiles(samples, 50, invoke), "R ");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "fixed-window benchmark failed: " << error.what() << '\n';
    return 1;
} catch (...) {
    std::cerr << "fixed-window benchmark failed with a nonstandard exception\n";
    return 1;
}
