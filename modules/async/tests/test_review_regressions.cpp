#include <doctest/doctest.h>

// Regressions for the 2026-11 review of the async layer. Each case failed
// before its fix; see the CHANGELOG entry for 3.3.1.

#include "aria/async/async_command.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/retry.hpp"
#include "aria/async/scope.hpp"
#include "aria/async/task.hpp"
#include "aria/async/timeout.hpp"
#include "aria/async/virtual_time_executor.hpp"
#include "aria/async/when_all.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace aria::async;
using namespace std::chrono_literals;

namespace {

Task<int> ready(int value) { co_return value; }

Task<long long> sum_ready_pairs(int iterations) {
    long long sum = 0;
    for (int i = 0; i < iterations; ++i) {
        auto [a, b] = co_await when_all(ready(i), ready(1));
        sum += a + b;
    }
    co_return sum;
}

/// Counts live instances, to observe what a registration keeps alive.
struct Tracked {
    static inline std::atomic<int> live{0};
    Tracked() { ++live; }
    Tracked(const Tracked& /*other*/) { ++live; }
    Tracked(Tracked&& /*other*/) noexcept { ++live; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --live; }
};

}  // namespace

TEST_CASE("when_all over synchronously completed children does not nest the caller") {
    // Before the fix the last child resumed the caller from inside
    // await_suspend, adding stack frames per iteration until the stack
    // overflowed (about 20k iterations on an 8 MiB stack).
    constexpr int iterations = 200'000;
    auto task = sum_ready_pairs(iterations);
    const long long expected =
        (static_cast<long long>(iterations) * (iterations - 1) / 2) + iterations;
    CHECK(task.blocking_get() == expected);
}

TEST_CASE("LatestOnly: a superseded invocation never publishes its result") {
    VirtualTimeExecutor vt;
    IExecutor& executor = vt;
    AsyncCommand<std::string, std::string> search(executor, executor,
        [&](CancellationToken, std::string query) -> Task<std::string> {
            // Not cooperative: finishes regardless of cancellation.
            co_await schedule_after(vt, query == "a" ? 100ms : 10ms);
            co_return "results for " + query;
        }, AsyncCommandPolicy::LatestOnly);

    search.execute("a");
    vt.advance_by(1ms);
    search.execute("ab");
    vt.advance_by(500ms);
    vt.run_until_idle();
    REQUIRE(search.last_result.peek().has_value());
    CHECK(*search.last_result.peek() == "results for ab");
    CHECK_FALSE(search.is_executing.peek());
}

TEST_CASE("LatestOnly: a superseded failure does not overwrite last_error") {
    VirtualTimeExecutor vt;
    IExecutor& executor = vt;
    AsyncCommand<int, int> command(executor, executor,
        [&](CancellationToken, int value) -> Task<int> {
            co_await schedule_after(vt, value == 1 ? 100ms : 10ms);
            if (value == 1) {
                throw std::runtime_error("stale failure");
            }
            co_return value;
        }, AsyncCommandPolicy::LatestOnly);

    command.execute(1);
    vt.advance_by(1ms);
    command.execute(2);
    vt.advance_by(500ms);
    vt.run_until_idle();
    CHECK_FALSE(command.last_error.peek().has_value());
    REQUIRE(command.last_result.peek().has_value());
    CHECK(*command.last_result.peek() == 2);
}

TEST_CASE("CoroutineScope torn down on the UI thread does not wait for its UI hop") {
    MainThreadExecutor ui;
    ui.drain();  // bind the owner thread

    struct ViewModel {
        int result = 0;
        CoroutineScope scope;
    };
    auto vm = std::make_unique<ViewModel>();
    auto* raw = vm.get();
    std::atomic<bool> unwound{false};
    vm->scope.launch([raw, &ui, &unwound](CancellationToken) -> Task<void> {
        struct Mark {
            std::atomic<bool>& flag;
            ~Mark() { flag = true; }
        } mark{unwound};
        co_await schedule_on(ui);  // parked in the UI queue
        raw->result = 42;          // must never run on a destroyed VM
    });

    const auto started = std::chrono::steady_clock::now();
    vm.reset();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < 1s);  // previously the full 5 s join timeout
    CHECK(unwound.load());
    ui.drain();  // the stale queue entry is now a no-op (ASan: no UAF)
}

TEST_CASE("schedule_on observes cancellation only inside a scope or with a token") {
    MainThreadExecutor ui;
    ui.drain();

    SUBCASE("explicit token: cancelled hop throws") {
        CancellationSource source;
        bool cancelled = false;
        auto task = [&] -> Task<void> {
            try {
                co_await schedule_on(ui, source.token());
            } catch (const OperationCancelled&) {
                cancelled = true;
            }
        };
        auto running = task();
        running.start();
        source.cancel();
        CHECK(cancelled);
        CHECK(running.done());
        ui.drain();  // the stale entry runs as a no-op
    }

    SUBCASE("none() keeps a cleanup hop alive after scope cancellation") {
        CoroutineScope scope;
        bool published = false;
        scope.launch([&](CancellationToken token) -> Task<void> {
            co_await token;
            co_await schedule_on(ui, CancellationToken::none());
            published = true;
        });
        scope.cancel();
        CHECK_FALSE(published);
        ui.drain();
        CHECK(published);
        CHECK(scope.cancel_and_join(1s));
    }

    SUBCASE("no scope, no token: plain hop") {
        bool hopped = false;
        auto task = [&] -> Task<void> {
            co_await schedule_on(ui);
            hopped = true;
        };
        auto running = task();
        running.start();
        CHECK_FALSE(hopped);
        ui.drain();
        CHECK(hopped);
    }
}

TEST_CASE("schedule_on to a discarding executor resumes the coroutine as cancelled") {
    bool cancelled = false;
    auto frame_owner = std::make_shared<int>(0);
    std::weak_ptr<int> frame_probe = frame_owner;
    std::optional<Task<void>> task;
    {
        auto ui = std::make_unique<MainThreadExecutor>();
        auto body = [&](std::shared_ptr<int> owned) -> Task<void> {
            try {
                co_await schedule_on(*ui);
            } catch (const OperationCancelled&) {
                cancelled = true;
            }
            (void)owned;
        };
        task.emplace(body(std::move(frame_owner)));
        task->start();
        CHECK_FALSE(cancelled);
        ui.reset();  // destroys the queued resumption without running it
    }
    CHECK(cancelled);
    CHECK(task->done());
    task.reset();
    CHECK(frame_probe.expired());
}

TEST_CASE("schedule_on continues on the target executor's thread") {
    // A worker that picks the resumption up before await_suspend returns
    // must still run the continuation itself, with or without cancellation.
    ThreadPoolExecutor pool{4};
    const auto caller = std::this_thread::get_id();
    std::atomic<int> on_caller{0};
    std::atomic<int> hopped{0};
    auto hop_once = [&] -> Task<void> {
        co_await schedule_on(pool);
        if (std::this_thread::get_id() == caller) {
            ++on_caller;
        }
        ++hopped;
    };
    constexpr int count = 2'000;
    std::vector<Task<void>> plain;
    plain.reserve(count);
    for (int i = 0; i < count; ++i) {
        plain.push_back(hop_once());
        plain.back().start();
    }
    CoroutineScope scope;  // cancellable path through the ambient token
    for (int i = 0; i < count; ++i) {
        scope.launch([&](CancellationToken) -> Task<void> { co_await hop_once(); });
    }
    pool.wait_idle();
    CHECK(scope.cancel_and_join(5s));
    CHECK(hopped.load() == 2 * count);
    CHECK(on_caller.load() == 0);
}

TEST_CASE("schedule_on to an executor that rejects work declines to suspend") {
    // Models a closed executor: the resumption is destroyed inside post().
    struct Discarding final : IExecutor {
        void post(std::function<void()> /*fn*/) override {}
    } discarding;
    bool cancelled = false;
    auto task = [&] -> Task<void> {
        try {
            co_await schedule_on(discarding);
        } catch (const OperationCancelled&) {
            cancelled = true;
        }
    };
    auto running = task();
    running.start();
    CHECK(cancelled);
    CHECK(running.done());
}

TEST_CASE("CancellationCallback withdraws its registration") {
    CancellationSource source;
    auto token = source.token();
    int fired = 0;
    {
        CancellationCallback scoped{token, [&] { ++fired; }};
        CHECK(scoped.registered());
    }
    CancellationCallback kept{token, [&] { fired += 10; }};
    source.cancel();
    CHECK(fired == 10);

    SUBCASE("already cancelled: runs immediately and is not registered") {
        int immediate = 0;
        CancellationCallback late{token, [&] { ++immediate; }};
        CHECK(immediate == 1);
        CHECK_FALSE(late.registered());
    }
}

TEST_CASE("CancellationCallback withdrawal waits for a callback running elsewhere") {
    CancellationSource source;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    auto registration = std::make_unique<CancellationCallback>(source.token(), [&] {
        entered = true;
        while (!release) {
            std::this_thread::yield();
        }
        finished = true;
    });
    std::thread canceller([&] { source.cancel(); });
    while (!entered) {
        std::this_thread::yield();
    }
    std::thread releaser([&] {
        std::this_thread::sleep_for(20ms);
        release = true;
    });
    registration.reset();  // must not return while the callback still runs
    CHECK(finished.load());
    canceller.join();
    releaser.join();
}

TEST_CASE("bounded waits on a long-lived token do not accumulate registrations") {
    VirtualTimeExecutor vt;
    CancellationSource parent;
    Tracked::live = 0;
    auto loop = [&](int iterations) -> Task<void> {
        for (int i = 0; i < iterations; ++i) {
            auto value = co_await with_timeout(parent.token(), vt, 10ms,
                [](CancellationToken) -> Task<Tracked> { co_return Tracked{}; },
                OnTimeout::Fail);
            (void)value;
            co_await with_timeout(parent.token(), vt, 10ms,
                [](CancellationToken) -> Task<void> { co_return; });
        }
    };
    auto task = loop(2'000);
    task.start();
    vt.advance_by(1s);
    vt.run_until_idle();
    REQUIRE(task.done());
    // Before the fix every call left a parent callback that retained its
    // race slot (and the Tracked result) until the parent was cancelled.
    CHECK(Tracked::live.load() == 0);

    CoroutineScope long_lived;
    for (int i = 0; i < 1'000; ++i) {
        CoroutineScope child{long_lived.token()};
    }
    long_lived.cancel();  // nothing stale left to run
    CHECK(long_lived.cancel_and_join(1s));
}
