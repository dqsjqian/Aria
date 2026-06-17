#pragma once

#include "aria/callback_boundary.hpp"
#include "aria/scheduler.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace aria::async {

/// Abstract executor interface — schedules a callable to run "somewhere".
///
/// Inherits virtually from `aria::IScheduler` so that role/capability
/// introspection is uniform across all schedulers in the framework
/// (executors, dispatchers, timers). The legacy two-bool capability
/// surface (`is_safe_graph_executor()` / `is_safe_worker_executor()`)
/// is preserved as thin views over `caps()` so existing IExecutor
/// subclasses keep compiling unchanged; a built-in executor that wants
/// to override capabilities should prefer overriding `caps()`.
///
/// The default capability set is `Post | GraphSafe | WorkerSafe`,
/// matching the historical "permissive" defaults. Specific built-in
/// executors override `caps()` to drop a flag they cannot uphold
/// (e.g. `ThreadPoolExecutor` drops `GraphSafe`).
class IExecutor : public virtual aria::IScheduler {
public:
    ~IExecutor() override = default;

    /// Legacy / canonical executor entry point. Implementations override
    /// this; the unified `IScheduler::schedule(fn)` is wired to it.
    virtual void post(std::function<void()> fn) = 0;

    // ── IScheduler bridge ────────────────────────────────────────────
    [[nodiscard]] aria::SchedulerCaps caps() const noexcept override {
        return aria::SchedulerCaps::Post
             | aria::SchedulerCaps::GraphSafe
             | aria::SchedulerCaps::WorkerSafe;
    }
    void schedule(std::function<void()> fn) override {
        post(std::move(fn));
    }

    // ── Legacy capability shims (kept for source compatibility) ──────
    /// True iff this executor is safe to use as the graph-thread (UI)
    /// executor. Reads `caps()`; override `caps()` in subclasses, not
    /// this method, unless you are a third-party implementation
    /// migrating gradually.
    [[nodiscard]] virtual bool is_safe_graph_executor() const noexcept {
        return aria::has_caps(*this, aria::SchedulerCaps::GraphSafe);
    }

    /// True iff this executor can host worker tasks. Reads `caps()`;
    /// override `caps()` in subclasses.
    [[nodiscard]] virtual bool is_safe_worker_executor() const noexcept {
        return aria::has_caps(*this, aria::SchedulerCaps::WorkerSafe);
    }
};

/// Thread pool executor.
class ThreadPoolExecutor : public IExecutor {
public:
    explicit ThreadPoolExecutor(std::size_t threads = std::thread::hardware_concurrency())
        : stop_(false) {
        if (threads == 0) threads = 1;
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this]() { worker_loop_(); });
        }
    }

    ~ThreadPoolExecutor() override {
        // Drain: wait until the queue is empty AND no worker is running a
        // task.  Detached coroutines that hop off to another executor will
        // re-post back to us when they resume; each post increments
        // `inflight_`, so as long as we wait on `inflight_ == 0` we won't
        // tear down the threadpool out from under a pending resume.
        //
        // CONTRACT: callers MUST ensure no new work is posted to this pool
        // after they begin destroying it.  The standard pattern is:
        //   1. Cancel every CoroutineScope that launches work on this pool.
        //   2. Allow cancelled coroutines to throw/unwind (they may still
        //      post one final resume — `wait_idle` will wait for it).
        //   3. Destroy the pool.
        wait_idle();
        {
            std::lock_guard lk(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    void post(std::function<void()> fn) override {
        {
            std::lock_guard lk(mutex_);
            queue_.push(std::move(fn));
            inflight_.fetch_add(1, std::memory_order_relaxed);
        }
        cv_.notify_one();
    }

    /// Worker pool: NOT safe as the graph executor. Property writes
    /// from a pool thread would race the reactive graph's owner-thread
    /// invariant. Caps advertise Post + WorkerSafe + Autonomous (no
    /// pump required); GraphSafe is explicitly absent.
    [[nodiscard]] aria::SchedulerCaps caps() const noexcept override {
        return aria::SchedulerCaps::Post
             | aria::SchedulerCaps::WorkerSafe
             | aria::SchedulerCaps::Autonomous;
    }

    [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

    /// Block until queue is drained AND no worker is currently running a
    /// task.  Uses a condition variable rather than a bounded spin-sleep so
    /// long tasks don't race the destructor.
    void wait_idle() {
        std::unique_lock lk(mutex_);
        idle_cv_.wait(lk, [this] {
            return queue_.empty() && inflight_.load() == 0;
        });
    }

private:
    void worker_loop_() {
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock lk(mutex_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                fn = std::move(queue_.front());
                queue_.pop();
            }
            try {
                fn();
            } catch (...) {
                aria::report_callback_failure(
                    std::string_view{"executor.thread_pool.worker"},
                    std::current_exception());
            }
            // Decrement AFTER the task has finished (not when we dequeued).
            if (inflight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // Last one — notify a possibly-waiting destructor.
                std::lock_guard lk(mutex_);
                idle_cv_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;       // wakes workers
    std::condition_variable idle_cv_;  // wakes wait_idle()
    std::atomic<int> inflight_{0};
    bool stop_;
};

/// Inline executor — runs callable synchronously on the calling thread.
///
/// `InlineExecutor` is suitable for **single-threaded** scenarios where
/// every actor (graph, worker, dispatcher) lives on the same thread, and
/// for use as a worker executor in tests. It must NOT be used as the
/// *graph-thread (UI) executor* together with a multi-threaded worker:
/// when the coroutine hops back via `co_await schedule_on(ui)` after
/// running on the worker, `InlineExecutor::post(fn)` would synchronously
/// run `fn` on the worker thread, tripping the reactive graph
/// thread-affinity assert.
///
/// `AsyncCommand` enforces this at compile time via the executor traits
/// (`is_safe_graph_executor_v` / `is_safe_worker_executor_v`); see
/// `executor_traits.hpp` and the static_asserts in `async_command.hpp`.
class InlineExecutor : public IExecutor {
public:
    /// Synchronously runs `fn` on the caller's thread. Exceptions are
    /// captured and reported via the unified callback-failure sink so
    /// the inline path matches the contract of every other executor in
    /// the framework — failures never escape `post()`.
    void post(std::function<void()> fn) override {
        if (!fn) return;
        try {
            fn();
        } catch (...) {
            aria::report_callback_failure(
                std::string_view{"executor.inline.post"},
                std::current_exception());
        }
    }

    // Inline runs synchronously on the caller's thread, so it cannot
    // claim main-thread affinity, pumping, or autonomy. The specific
    // "Inline graph + non-Inline worker" race is rejected independently
    // by an explicit dynamic_cast in detail::check_executor_safety_runtime
    // — see async_command.hpp.
    [[nodiscard]] aria::SchedulerCaps caps() const noexcept override {
        return aria::SchedulerCaps::Post
             | aria::SchedulerCaps::GraphSafe
             | aria::SchedulerCaps::WorkerSafe;
    }
};

/// Main-thread executor — queues callables for later execution on the
/// thread that "owns" the executor (typically the application's main
/// thread or a test thread).
///
/// `MainThreadExecutor` is the canonical **graph-thread executor** for
/// any scenario that mixes a thread-pool worker with reactive Property
/// writes. The owner thread is established lazily by the first call to
/// `drain()`, `pump_until()` or `pump_one()`; subsequent attempts to
/// pump from a different thread trip a debug assert (and a runtime
/// throw in Release).
///
/// Usage (test):
///
///   MainThreadExecutor ui;             // declared on the test thread
///   ThreadPoolExecutor pool{4};
///
///   AsyncCommand<int, int> cmd{ui, pool, ...};
///   cmd.execute(7);
///   // Pump until the coroutine has marshalled its Property writes
///   // back to the graph thread.
///   REQUIRE(ui.pump_until([&]{ return !cmd.is_executing.get(); }));
///
/// Usage (console / headless app):
///
///   MainThreadExecutor main_loop;
///   set_main_executor(main_loop);
///   // ... wire up your ViewModels, Commands, etc. ...
///   while (running) main_loop.run_one();   // blocks until next post
///
/// Thread-safety:
///   * `post(fn)` is callable from ANY thread.
///   * `drain()` / `pump_until()` / `pump_one()` / `run_one()` are owner-
///     thread-only; the first such call locks the owner identity.
///   * `pending()`, `is_owner_thread()`, `clear()` are thread-safe.
class MainThreadExecutor : public IExecutor {
public:
    void post(std::function<void()> fn) override {
        {
            std::lock_guard lk(m_);
            queue_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    /// Main-thread executor: safe in both reactive roles, plus
    /// Pumpable (drain/pump_until/run_one) and MainThread (owner-thread
    /// affinity is enforced after first pump).
    [[nodiscard]] aria::SchedulerCaps caps() const noexcept override {
        return aria::SchedulerCaps::Post
             | aria::SchedulerCaps::GraphSafe
             | aria::SchedulerCaps::WorkerSafe
             | aria::SchedulerCaps::MainThread
             | aria::SchedulerCaps::Pumpable;
    }

    [[nodiscard]] bool is_main_thread() const noexcept override {
        return is_owner_thread();
    }

    /// Run callables. Drains recursively: any task that posts further
    /// callables on this executor (typical of coroutine resumption that
    /// schedules a follow-up on the same thread) will be picked up in
    /// the same call. Returns the total number of callables executed.
    ///
    /// Owner-thread-only.
    std::size_t drain() {
        bind_owner_();
        std::size_t total = 0;
        while (true) {
            std::vector<std::function<void()>> local;
            {
                std::lock_guard lk(m_);
                if (queue_.empty()) break;
                local.reserve(queue_.size());
                std::move(queue_.begin(), queue_.end(), std::back_inserter(local));
                queue_.clear();
            }
            for (auto& fn : local) {
                try {
                    fn();
                } catch (...) {
                    aria::report_callback_failure(
                        std::string_view{"executor.main_thread.drain"},
                        std::current_exception());
                }
                ++total;
            }
        }
        return total;
    }

    /// Pump until `predicate()` returns true OR `timeout` elapses, then
    /// return. Blocks the owner thread on a condition variable when the
    /// queue is empty — no spin sleeping. Returns true iff predicate
    /// became true within the deadline.
    ///
    /// Owner-thread-only.
    template<typename Pred>
    bool pump_until(Pred predicate,
                    std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
        bind_owner_();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            drain();
            if (predicate()) return true;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            std::unique_lock lk(m_);
            cv_.wait_until(lk, deadline, [this]{ return !queue_.empty(); });
            // Loop: if predicate is already true the next drain is a no-op
            // and we return true. If we woke on timeout the next iteration
            // will see `now >= deadline` and bail.
        }
    }

    /// Run exactly one callable, blocking the owner thread until one is
    /// available. Suitable as the body of a console app's main loop.
    ///
    /// Owner-thread-only.
    void run_one() {
        bind_owner_();
        std::function<void()> fn;
        {
            std::unique_lock lk(m_);
            cv_.wait(lk, [this]{ return !queue_.empty(); });
            fn = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            fn();
        } catch (...) {
            aria::report_callback_failure(
                std::string_view{"executor.main_thread.run_one"},
                std::current_exception());
        }
    }

    /// True if called from the thread that owns this executor (or if
    /// no owner has been bound yet).
    [[nodiscard]] bool is_owner_thread() const noexcept {
        const auto id = owner_.load(std::memory_order_acquire);
        return id == std::thread::id{} || id == std::this_thread::get_id();
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        std::lock_guard lk(m_);
        return queue_.size();
    }

    /// Drop all pending callables without running them. Owner-thread-only.
    void clear() noexcept {
        bind_owner_();
        std::lock_guard lk(m_);
        queue_.clear();
    }

private:
    void bind_owner_() noexcept {
        std::thread::id expected{};
        const auto self = std::this_thread::get_id();
        if (owner_.compare_exchange_strong(expected, self,
                                           std::memory_order_acq_rel)) {
            return;  // we just claimed ownership
        }
        // Already bound — must match.
        assert(expected == self
               && "MainThreadExecutor pumped from a non-owner thread. "
                  "post() is fine from any thread, but drain/pump/run_one "
                  "must run on the thread that originally bound the executor.");
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::atomic<std::thread::id> owner_{};
};

/// Schedule a coroutine to resume on the given executor.
inline auto schedule_on(IExecutor& exec) {
    struct Awaiter {
        IExecutor& exec;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) const {
            exec.post([h]() mutable { h.resume(); });
        }
        void await_resume() const noexcept {}
    };
    return Awaiter{exec};
}

}  // namespace aria::async
