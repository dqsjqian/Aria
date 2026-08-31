// bench_command — Property / Computed / EventBus / Container hot paths.
//
// The "command" name refers to the *imperative* surface — single-step
// reads/writes that an MVVM command would issue per click. Derived
// list and async benches live in their own binaries (see
// benchmark/CMakeLists.txt).

#include "aria/aria.hpp"
#include "aria/abi/version.hpp"
#include "aria/runtime/container.hpp"
#include "aria/runtime/event_bus.hpp"

#include "bench_common.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace aria;
using namespace aria::runtime;
using namespace aria_bench;

int main() {
    banner(std::string("aria v") + abi::version_string + " — bench_command");

    // Property::get
    {
        Property<int> p(42);
        const int N = 10'000'000;
        aria_bench::Sink sink;
        double ns = measure_ns(N, [&](int) { sink.feed(p.get()); });
        (void)sink.value();
        row("Property<int>::get()", ns, N);
    }

    // Property::set no observers
    {
        Property<int> p(0);
        const int N = 5'000'000;
        double ns = measure_ns(N, [&](int i) { p.set(i); });
        row("Property<int>::set() (no observers)", ns, N);
    }

    // Property::set 1 observer
    {
        Property<int> p(0);
        aria_bench::Sink sink;
        auto sub = p.on_changed([&](const int& v) { sink.feed(v); });
        const int N = 1'000'000;
        double ns = measure_ns(N, [&](int i) { p.set(i); });
        (void)sink.value();
        row("Property<int>::set() with 1 observer", ns, N);
    }

    // Property::set 10 observers
    {
        Property<int> p(0);
        aria_bench::Sink sink;
        std::vector<Subscription> subs;
        for (int i = 0; i < 10; ++i)
            subs.push_back(p.on_changed([&](const int& v) { sink.feed(v); }));
        const int N = 500'000;
        double ns = measure_ns(N, [&](int i) { p.set(i); });
        (void)sink.value();
        row("Property<int>::set() with 10 observers", ns, N);
    }

    // Subscribe + auto-unsubscribe cycle
    {
        Property<int> p(0);
        const int N = 1'000'000;
        double ns = measure_ns(N, [&](int) {
            auto sub = p.on_changed([](const int&) {});
        });
        row("Subscribe + auto-unsubscribe cycle", ns, N);
    }

    // Computed chain x5
    {
        Property<int> a(1);
        Computed<int> c1([&]{ return a.get() * 2; });
        Computed<int> c2([&]{ return c1.get() + 1; });
        Computed<int> c3([&]{ return c2.get() * 2; });
        Computed<int> c4([&]{ return c3.get() + 1; });
        Computed<int> c5([&]{ return c4.get() * 2; });
        aria_bench::Sink sink;
        const int N = 200'000;
        double ns = measure_ns(N, [&](int i) {
            a.set(i);
            sink.feed(c5.get());
        });
        (void)sink.value();
        row("Computed chain x5 (auto-tracked)", ns, N);
    }

    // EventBus publish
    {
        struct E { int v; };
        EventBus bus;
        aria_bench::Sink sink;
        auto sub = bus.subscribe<E>([&](const E& e) { sink.feed(e.v); });
        const int N = 1'000'000;
        double ns = measure_ns(N, [&](int i) { bus.publish(E{i}); });
        (void)sink.value();
        row("EventBus::publish (1 subscriber)", ns, N);
    }

    // Container resolve singleton
    {
        struct ILogger { virtual ~ILogger() = default; virtual void log() = 0; };
        struct Logger : ILogger { void log() override {} };
        Container c;
        c.register_singleton<ILogger, Logger>();
        const int N = 1'000'000;
        double ns = measure_ns(N, [&](int) {
            auto p = c.resolve<ILogger>();
            (void)p;
        });
        row("Container::resolve<Singleton>", ns, N);
    }

    // Batch update vs individual writes
    {
        Property<int> p(0);
        aria_bench::Sink sink;
        auto sub = p.on_changed([&](const int& v) { sink.feed(v); });

        const int N = 100'000;
        double ns_indiv = measure_ns(N, [&](int i) {
            for (int j = 0; j < 10; ++j) p.set(i * 10 + j);
        });
        double ns_batch = measure_ns(N, [&](int i) {
            reactive::batch([&]{
                for (int j = 0; j < 10; ++j) p.set(i * 10 + j);
            });
        });
        (void)sink.value();
        row("10 sets individually (notify each)", ns_indiv, N);
        row("10 sets in reactive::batch (notify once)", ns_batch, N);
        std::cout << "  -> batch speedup: " << std::fixed << std::setprecision(2)
                  << (ns_indiv / ns_batch) << "x\n";
    }

    // Edge-set churn (kept here as it stresses Computed's per-recompute
    // edge reconciliation, the hottest imperative path).
    {
        constexpr int kPool       = 128;
        constexpr int kDepsPerRun = 8;
        std::vector<std::unique_ptr<Property<int>>> pool;
        pool.reserve(kPool);
        for (int i = 0; i < kPool; ++i)
            pool.emplace_back(std::make_unique<Property<int>>(i));

        Property<int> tick(0);

        Computed<long long> stable([&]{
            (void)tick.get();
            long long s = 0;
            for (int k = 0; k < kDepsPerRun; ++k)
                s += pool[static_cast<std::size_t>(k)]->get();
            return s;
        });
        (void)stable.get();

        const int N = 200'000;
        aria_bench::Sink sink;
        double ns_stable = measure_ns(N, [&](int i) {
            tick.set(i);
            sink.feed(stable.get());
        });
        (void)sink.value();
        row("Computed recompute, STABLE deps (8/8 same)", ns_stable, N);

        int window = 0;
        Computed<long long> churn([&]{
            (void)tick.get();
            long long s = 0;
            const int start = window;
            for (int k = 0; k < kDepsPerRun; ++k) {
                s += pool[static_cast<std::size_t>((start + k) % kPool)]->get();
            }
            return s;
        });
        (void)churn.get();

        double ns_churn = measure_ns(N, [&](int i) {
            window = (window + 1) % kPool;
            tick.set(i);
            sink.feed(churn.get());
        });
        (void)sink.value();
        row("Computed recompute, CHURN deps (different 8/128)", ns_churn, N);
        std::cout << "  -> churn overhead vs stable: " << std::fixed
                  << std::setprecision(2)
                  << (ns_churn / ns_stable) << "x\n";
    }

    // -------------------------------------------------------------------
    //  Percentile snapshot — feeds nightly check-bench.sh thresholds.
    //  Two indicators are pinned:
    //    1. Property::set with one observer  — the canonical reactive
    //       hot path; regression here means notify_changed got slower.
    //    2. Computed chain x5                — multi-hop dirty
    //       propagation; regression means graph traversal got slower.
    // -------------------------------------------------------------------
    {
        Property<int> p(0);
        aria_bench::Sink sink;
        auto sub = p.on_changed([&](const int& v) { sink.feed(v); });

        constexpr int kSamples      = 256;
        constexpr int kOpsPerSample = 1'000;
        auto stats = measure_percentiles(kSamples, kOpsPerSample,
            [&](int i) { p.set(i); });
        (void)sink.value();
        row_pct("Property<int>::set() + 1 observer", stats);
    }
    {
        Property<int> a(1);
        Computed<int> c1([&]{ return a.get() * 2; });
        Computed<int> c2([&]{ return c1.get() + 1; });
        Computed<int> c3([&]{ return c2.get() * 2; });
        Computed<int> c4([&]{ return c3.get() + 1; });
        Computed<int> c5([&]{ return c4.get() * 2; });
        aria_bench::Sink sink;

        constexpr int kSamples      = 128;
        constexpr int kOpsPerSample = 500;
        auto stats = measure_percentiles(kSamples, kOpsPerSample,
            [&](int i) { a.set(i); sink.feed(c5.get()); });
        (void)sink.value();
        row_pct("Computed chain x5 (auto-tracked)", stats);
    }

    std::cout << "\n=== done ===\n";
    return 0;
}
