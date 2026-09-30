#pragma once

// VirtualTimeExecutor — deterministic time-based scheduler for tests.
//
// Production code uses real wall-clock executors (ThreadPoolExecutor /
// SimpleDispatcher).  Async tests that involve `debounce(300ms)` or
// `retry_with_backoff(...)` would otherwise have to actually sleep, making the
// test suite slow and flaky.
//
// VirtualTimeExecutor decouples "logical time" from wall-clock time.  Tasks
// scheduled with a delay sit in a sorted queue keyed by virtual deadline;
// `advance_by(n)` jumps the clock forward and synchronously fires every task
// whose deadline has passed.  No real sleeping, no threads.
//
// Usage:
//
//   VirtualTimeExecutor vt;
//   bool fired = false;
//   vt.post_after(500ms, [&]{ fired = true; });
//   vt.advance_by(499ms);  CHECK(!fired);
//   vt.advance_by(1ms);    CHECK(fired);
//
// Combined with `schedule_on` / `schedule_after` it produces fully
// deterministic coroutine tests:
//
//   Task<int> body(VirtualTimeExecutor& vt) {
//       co_await schedule_after(vt, 300ms);
//       co_return 42;
//   }
//   auto t = body(vt);
//   vt.advance_by(300ms);
//   CHECK(t.get() == 42);

#include "aria/async/executor.hpp"
#include "aria/property_ops.hpp"
#include "aria/callback_boundary.hpp"

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>

namespace aria::async {

class VirtualTimeExecutor : public IExecutor, public IDelayedScheduler {
public:
    using clock = std::chrono::steady_clock;
    using duration = std::chrono::milliseconds;

    VirtualTimeExecutor() = default;
    VirtualTimeExecutor(const VirtualTimeExecutor&) = delete;
    VirtualTimeExecutor& operator=(const VirtualTimeExecutor&) = delete;
    VirtualTimeExecutor(VirtualTimeExecutor&&) = delete;
    VirtualTimeExecutor& operator=(VirtualTimeExecutor&&) = delete;

    ~VirtualTimeExecutor() override {
        decltype(queue_) retired;
        {
            std::scoped_lock lk(m_);
            closed_ = true;
            std::swap(queue_, retired);
        }
        // User captures can reenter post while the closed state is still live.
    }

    /// Capabilities: Post (immediate), Delay (deadline-keyed), GraphSafe
    /// + WorkerSafe (single-threaded virtual clock — no thread races),
    /// Pumpable (advance_by/run_until_idle drive progress; no
    /// autonomous threads). Explicit override is required because both
    /// `IExecutor` and `IDelayedScheduler` provide a `caps()` of their
    /// own and the multiple-inheritance lookup would otherwise be
    /// ambiguous.
    [[nodiscard]] aria::SchedulerCaps caps() const noexcept override {
        return aria::SchedulerCaps::Post
             | aria::SchedulerCaps::Delay
             | aria::SchedulerCaps::GraphSafe
             | aria::SchedulerCaps::WorkerSafe
             | aria::SchedulerCaps::Pumpable;
    }
    void schedule(std::function<void()> fn) override { post(std::move(fn)); }
    void schedule_after(std::chrono::milliseconds delay,
                        std::function<void()> fn) override {
        post_after(delay, std::move(fn));
    }

    /// IExecutor: schedule "now" — runs at the current virtual time
    /// when the next advance_by()/run_until_idle() is called.
    void post(std::function<void()> fn) override {
        post_after(duration{0}, std::move(fn));
    }

    /// Schedule to run after `delay` of *virtual* time.
    /// (Also implements IDelayedScheduler.)
    void post_after(duration delay, std::function<void()> fn) override {
        if (!fn) { return; }
        std::lock_guard lk(m_);
        if (closed_) { return; }
        queue_.push(Entry{add_delay(now_, delay), ++seq_, std::move(fn)});
    }

    /// Current virtual time (since construction).
    [[nodiscard]] duration now() const noexcept {
        std::lock_guard lk(m_);
        return now_;
    }

    /// Number of scheduled tasks not yet fired.
    [[nodiscard]] std::size_t pending() const noexcept {
        std::lock_guard lk(m_);
        return queue_.size();
    }

    /// Advance virtual time by `delta`, firing tasks in deadline order.
    /// Returns the number of tasks fired.
    std::size_t advance_by(duration delta) {
        if (delta < duration::zero()) {
            throw std::invalid_argument("VirtualTimeExecutor cannot move time backwards");
        }
        return advance_to(add_delay(now(), delta));
    }

    /// Advance to an absolute virtual time `target` (must be >= now()).
    std::size_t advance_to(duration target) {
        if (target < now()) {
            throw std::invalid_argument("VirtualTimeExecutor cannot move time backwards");
        }
        std::size_t fired = 0;
        while (true) {
            std::function<void()> fn;
            {
                std::lock_guard lk(m_);
                if (queue_.empty() || queue_.top().deadline > target) {
                    // A callback may itself advance time beyond our target.
                    if (target > now_) { now_ = target; }
                    break;
                }
                const auto deadline = queue_.top().deadline;
                fn = std::move(queue_.top().fn);
                queue_.pop();
                if (deadline > now_) { now_ = deadline; }
            }
            // Run outside lock — task may schedule new tasks.
            try {
                fn();
            } catch (...) {
                aria::report_callback_failure(
                    std::string_view{"executor.virtual_time.advance"},
                    std::current_exception());
            }
            ++fired;
        }
        return fired;
    }

    /// Run everything currently queued without advancing time beyond the
    /// last deadline.  Useful when you only want to drain "post()" tasks
    /// (delay == 0) without simulating a time jump.
    std::size_t run_until_idle() {
        std::size_t fired = 0;
        while (true) {
            std::function<void()> fn;
            {
                std::lock_guard lk(m_);
                if (queue_.empty()) break;
                const auto deadline = queue_.top().deadline;
                fn = std::move(queue_.top().fn);
                queue_.pop();
                if (deadline > now_) { now_ = deadline; }
            }
            try {
                fn();
            } catch (...) {
                aria::report_callback_failure(
                    std::string_view{"executor.virtual_time.run_until_idle"},
                    std::current_exception());
            }
            ++fired;
        }
        return fired;
    }

    /// Drop all scheduled tasks without firing them.
    void clear() noexcept {
        std::priority_queue<Entry, std::vector<Entry>, Cmp> retired;
        {
            std::lock_guard lk(m_);
            std::swap(queue_, retired);
        }
        // Capture destructors can safely schedule new work after unlocking.
    }

private:
    static duration add_delay(duration now, duration delay) noexcept {
        if (delay <= duration::zero()) { return now; }
        return now > duration::max() - delay ? duration::max() : now + delay;
    }

    struct Entry {
        duration       deadline;
        std::uint64_t  seq;   // tie-breaker so insertion order is preserved
        mutable std::function<void()> fn;
    };
    struct Cmp {
        bool operator()(const Entry& a, const Entry& b) const noexcept {
            if (a.deadline != b.deadline) return a.deadline > b.deadline;
            return a.seq > b.seq;
        }
    };

    mutable std::mutex m_;
    duration           now_{0};
    std::uint64_t      seq_{0};
    std::priority_queue<Entry, std::vector<Entry>, Cmp> queue_;
    bool closed_ = false;
};

/// Awaiter that resumes the coroutine after `delay` of virtual time.
///
///   co_await schedule_after(vt, 500ms);
inline auto schedule_after(VirtualTimeExecutor& vt,
                           VirtualTimeExecutor::duration delay) {
    struct Awaiter {
        VirtualTimeExecutor& vt;
        VirtualTimeExecutor::duration delay;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const {
            vt.post_after(delay, [h]() mutable { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    return Awaiter{vt, delay};
}

}  // namespace aria::async
