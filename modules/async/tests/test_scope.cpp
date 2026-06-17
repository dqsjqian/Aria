// Contract tests for the upgraded CoroutineScope (structured concurrency).
//
// Covers:
//   1. inflight_count tracking across launch -> drain.
//   2. cancel_and_join blocking semantics.
//   3. co_await join() awaitable semantics.
//   4. Destructor as structured-concurrency boundary (no detached task
//      outlives the scope under cooperative cancellation).
//   5. Parent-child cancellation linkage.
//   6. Error sink boundary: unhandled exceptions in launched bodies
//      route to the async error sink instead of vanishing.
//   7. OperationCancelled is silently absorbed, not reported as error.

#include <doctest/doctest.h>

#include "aria/async/async_error_sink.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/scope.hpp"
#include "aria/async/task.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace aria::async;

namespace {

// Spin until predicate is true or the deadline passes; returns predicate.
template <typename Pred>
bool wait_until(Pred p,
                std::chrono::milliseconds budget = std::chrono::milliseconds{1000}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!p()) {
        if (std::chrono::steady_clock::now() > deadline) return p();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return true;
}

// RAII helper: install an error sink for the test body, restore on exit.
class SinkGuard {
public:
    explicit SinkGuard(ErrorSink s) {
        prev_ = error_sink_();
        set_error_sink(std::move(s));
    }
    ~SinkGuard() { set_error_sink(prev_); }
    SinkGuard(const SinkGuard&) = delete;
    SinkGuard& operator=(const SinkGuard&) = delete;
private:
    ErrorSink prev_;
};

}  // namespace

TEST_CASE("CoroutineScope tracks inflight count across launch and drain") {
    CoroutineScope scope;
    CHECK(scope.inflight_count() == 0);

    // Cooperative coroutine that suspends on `co_await tok` so it can
    // observe cancellation cleanly. We deliberately avoid an
    // unresumable suspend (e.g. `co_await std::suspend_always{}`):
    // such a frame would leak across process exit and trip the C++
    // runtime's exit-time cleanup on strict platforms (notably
    // Windows MSYS2 UCRT64), masking real bugs in this test.
    auto started = std::make_shared<std::atomic<bool>>(false);
    scope.launch([started](CancellationToken tok) -> Task<void> {
        started->store(true, std::memory_order_release);
        while (true) {
            tok.throw_if_cancelled();
            co_await tok;  // resumed on cancel, then throws
        }
    });

    // Wait until the body has actually entered (i.e. the wrapper has
    // bumped inflight and parked on `co_await tok`). The wrapper
    // increments inflight before the body starts, so observing
    // `inflight_count() == 1` is also the correct post-launch state.
    REQUIRE(wait_until([&]{ return started->load(std::memory_order_acquire); }));
    CHECK(scope.inflight_count() == 1);

    // Cancel + drain cooperatively. The body responds to cancellation
    // promptly via the awaiter on `tok`, so a tight timeout is enough
    // to see the inflight count return to zero — no leaked frame.
    const bool drained = scope.cancel_and_join(std::chrono::milliseconds{500});
    CHECK(drained);
    CHECK(scope.inflight_count() == 0);
}

TEST_CASE("CoroutineScope: cancel_and_join drains a cooperatively cancellable task") {
    CoroutineScope scope;
    auto started = std::make_shared<std::atomic<bool>>(false);

    scope.launch([started](CancellationToken tok) -> Task<void> {
        started->store(true, std::memory_order_release);
        // Cooperative loop: probes cancellation between awaits.
        while (true) {
            tok.throw_if_cancelled();
            co_await tok;  // suspends until cancellation
        }
    });

    CHECK(wait_until([&] { return started->load(std::memory_order_acquire); }));
    CHECK(scope.inflight_count() == 1);

    const bool drained = scope.cancel_and_join(std::chrono::milliseconds{500});
    CHECK(drained);
    CHECK(scope.inflight_count() == 0);
}

TEST_CASE("CoroutineScope: co_await join() drains in-flight tasks") {
    CoroutineScope scope;
    auto worked = std::make_shared<std::atomic<int>>(0);

    for (int i = 0; i < 4; ++i) {
        scope.launch([worked](CancellationToken tok) -> Task<void> {
            (void)tok;
            worked->fetch_add(1, std::memory_order_acq_rel);
            co_return;
        });
    }

    // Wait for the wrappers to finish. We can't co_await from a sync
    // test body, so use blocking_get on a coroutine that awaits join().
    auto wait_task = [](CoroutineScope* s) -> Task<void> {
        co_await s->join_existing();
    }(&scope);
    wait_task.blocking_get();

    CHECK(worked->load() == 4);
    CHECK(scope.inflight_count() == 0);
}

TEST_CASE("CoroutineScope: dtor enforces structured-concurrency boundary") {
    auto seen_after_scope_destroyed =
        std::make_shared<std::atomic<bool>>(false);
    auto observed = std::make_shared<std::atomic<bool>>(false);

    {
        CoroutineScope scope;
        scope.launch([observed, seen_after_scope_destroyed](CancellationToken tok)
                         -> Task<void> {
            (void)seen_after_scope_destroyed;
            try {
                while (true) {
                    tok.throw_if_cancelled();
                    co_await tok;  // resumes on cancel, then throws
                }
            } catch (const OperationCancelled&) {
                observed->store(true, std::memory_order_release);
                throw;  // re-throw; wrapper absorbs.
            }
        });
        CHECK(scope.inflight_count() == 1);
    }  // ~CoroutineScope: cancels + joins + waits.

    // After scope teardown, the body MUST have observed cancellation and
    // exited. No detached zombie.
    CHECK(observed->load(std::memory_order_acquire));
}

TEST_CASE("CoroutineScope: parent cancellation propagates to child") {
    CancellationSource parent_src;
    CoroutineScope child{parent_src.token()};

    auto stopped = std::make_shared<std::atomic<bool>>(false);
    child.launch([stopped](CancellationToken tok) -> Task<void> {
        try {
            while (true) {
                tok.throw_if_cancelled();
                co_await tok;
            }
        } catch (const OperationCancelled&) {
            stopped->store(true, std::memory_order_release);
            throw;
        }
    });

    CHECK_FALSE(child.is_cancelled());
    parent_src.cancel();
    // Child must be cancelled now.
    CHECK(child.is_cancelled());

    const bool drained =
        child.cancel_and_join(std::chrono::milliseconds{500});
    CHECK(drained);
    CHECK(stopped->load(std::memory_order_acquire));
}

TEST_CASE("CoroutineScope: child outliving parent does not crash on parent cancel") {
    // If the child scope is destroyed before the parent ever cancels,
    // the parent's stored callback must safely no-op.
    CancellationSource parent_src;
    {
        CoroutineScope child{parent_src.token()};
        // Don't launch anything; just exercise destruction order.
    }
    // Now the child is gone; cancelling the parent must NOT crash.
    parent_src.cancel();
    CHECK(parent_src.is_cancelled());
}

TEST_CASE("CoroutineScope: launched body throwing std::exception routes to error sink") {
    std::mutex mu;
    std::vector<std::string> captured;
    SinkGuard guard{[&](std::string_view m) {
        std::lock_guard lk(mu);
        captured.emplace_back(m);
    }};

    {
        CoroutineScope scope;
        scope.launch([](CancellationToken) -> Task<void> {
            throw std::runtime_error{"boom-from-launch"};
            co_return;  // unreachable, present for the compiler
        });
        // Wait for the wrapper to drain naturally (no cancel needed).
        auto wait = [](CoroutineScope* s) -> Task<void> {
            co_await s->join_existing();
        }(&scope);
        wait.blocking_get();
    }

    bool found = false;
    {
        std::lock_guard lk(mu);
        for (auto& s : captured) {
            if (s.find("boom-from-launch") != std::string::npos) {
                found = true;
                break;
            }
        }
    }
    CHECK(found);
}

TEST_CASE("CoroutineScope: OperationCancelled is silently absorbed (no sink noise)") {
    std::atomic<int> sink_hits{0};
    SinkGuard guard{[&](std::string_view) {
        sink_hits.fetch_add(1, std::memory_order_acq_rel);
    }};

    {
        CoroutineScope scope;
        scope.launch([](CancellationToken tok) -> Task<void> {
            tok.throw_if_cancelled();
            co_await tok;
            tok.throw_if_cancelled();  // unreachable post-cancel resume
            co_return;
        });
        scope.cancel_and_join(std::chrono::milliseconds{500});
    }

    CHECK(sink_hits.load() == 0);
}
