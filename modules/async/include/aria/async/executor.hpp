#pragma once

#include "aria/callback_boundary.hpp"
#include "aria/scheduler.hpp"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
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
        try {
            for (std::size_t i = 0; i < threads; ++i) {
                workers_.emplace_back([this]() { worker_loop_(); });
            }
        } catch (...) {
            // A failed thread creation must not destroy joinable std::threads
            // (which terminates the process). Stop and join the started prefix.
            {
                std::scoped_lock lk(mutex_);
                stop_ = true;
            }
            wake_sequence_.fetch_add(1, std::memory_order_release);
            wake_sequence_.notify_all();
            for (auto& worker : workers_) {
                if (worker.joinable()) { worker.join(); }
            }
            throw;
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
        wake_sequence_.fetch_add(1, std::memory_order_release);
        wake_sequence_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    void post(std::function<void()> fn) override {
        {
            std::lock_guard lk(mutex_);
            queue_.push(std::move(fn));
            inflight_.fetch_add(1, std::memory_order_relaxed);
        }
        wake_sequence_.fetch_add(1, std::memory_order_release);
        wake_sequence_.notify_one();
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
    /// task. Atomic waiting observes the published count without a spin loop.
    void wait_idle() {
        while (true) {
            int pending = 0;
            {
                // Queue publication and its count increment are one locked
                // operation. Sample both before waiting outside the lock.
                std::lock_guard lk(mutex_);
                pending = inflight_.load(std::memory_order_acquire);
                if (queue_.empty() && pending == 0) { return; }
            }
            // Completion between unlock and wait changes the sampled value,
            // so a notification cannot be lost in that handoff window.
            inflight_.wait(pending, std::memory_order_acquire);
        }
    }

private:
    void worker_loop_() {
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock lk(mutex_);
                while (!stop_ && queue_.empty()) {
                    const auto sequence = wake_sequence_.load(std::memory_order_acquire);
                    lk.unlock();
                    wake_sequence_.wait(sequence, std::memory_order_acquire);
                    lk.lock();
                }
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
                // Atomic waiters need no queue-lock handshake on completion.
                inflight_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> queue_;
    std::mutex mutex_;
    // Publication epochs avoid the condition-variable re-lock handshake.
    // A worker samples the epoch while holding the queue lock, then waits
    // outside it: a post between unlock and wait changes the sampled value.
    std::atomic<std::uint64_t> wake_sequence_{0};
    // Reserved to retain the existing ThreadPoolExecutor object layout.
    [[maybe_unused]] std::condition_variable idle_cv_;
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
/// pump from a different thread throw std::logic_error in every build.
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
///   * `pending()` and `is_owner_thread()` are thread-safe.
///   * `clear()` is owner-thread-only, just like pumping.
class MainThreadExecutor : public IExecutor {
public:
    MainThreadExecutor() = default;
    MainThreadExecutor(const MainThreadExecutor&) = delete;
    MainThreadExecutor& operator=(const MainThreadExecutor&) = delete;
    MainThreadExecutor(MainThreadExecutor&&) = delete;
    MainThreadExecutor& operator=(MainThreadExecutor&&) = delete;

    ~MainThreadExecutor() override {
        std::deque<std::function<void()>> retired;
        {
            std::scoped_lock lk(m_);
            closed_ = true;
            retired.swap(queue_);
        }
        // Reject work posted by capture destructors while all members are live.
    }

    void post(std::function<void()> fn) override {
        if (!fn) { return; }
        {
            std::lock_guard lk(m_);
            if (closed_) { return; }
            queue_.push_back(std::move(fn));
        }
        // An already-bound owner cannot simultaneously be asleep in its own
        // pump. Foreign producers and an unbound executor must still wake it.
        if (owner_.load(std::memory_order_acquire) != std::this_thread::get_id()) {
            cv_.notify_one();
        }
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
            std::function<void()> single;
            std::vector<std::function<void()>> local;
            {
                std::lock_guard lk(m_);
                if (queue_.empty()) { break; }
                if (queue_.size() == 1) {
                    single = std::move(queue_.front());
                    queue_.pop_front();
                } else {
                    local.reserve(queue_.size());
                    std::move(queue_.begin(), queue_.end(), std::back_inserter(local));
                    queue_.clear();
                }
            }
            // The common coroutine handoff carries one callable. Keep it on
            // the stack rather than allocating a vector for every resume.
            // Multi-callable batches still detach together before callbacks.
            if (single) {
                invoke_drained(single);
                ++total;
            } else {
                for (auto& fn : local) {
                    invoke_drained(fn);
                    ++total;
                }
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
    void clear() {
        bind_owner_();
        std::deque<std::function<void()>> retired;
        {
            std::lock_guard lk(m_);
            retired.swap(queue_);
        }
        // Destroy user captures after unlocking: their destructors may post.

    }

private:
    static void invoke_drained(std::function<void()>& fn) {
        try {
            fn();
        } catch (...) {
            aria::report_callback_failure(
                std::string_view{"executor.main_thread.drain"},
                std::current_exception());
        }
    }

    void bind_owner_() {
        const auto self = std::this_thread::get_id();
        if (owner_.load(std::memory_order_acquire) == self) { return; }
        std::thread::id expected{};
        if (owner_.compare_exchange_strong(expected, self,
                                           std::memory_order_acq_rel)) {
            return;  // we just claimed ownership
        }
        // Already bound — must match.
        if (expected != self) {
            throw std::logic_error(
                "MainThreadExecutor must be pumped or cleared on its owner thread");
        }
    }

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::atomic<std::thread::id> owner_{};
    bool closed_ = false;
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
