#pragma once

#include "aria/async/cancellation.hpp"
#include "aria/async/task.hpp"
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
#include <optional>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
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

    /// Pending callables are destroyed, not run. A queued `schedule_on` hop
    /// is not dropped silently: destroying it resumes its coroutine on this
    /// thread with OperationCancelled, so the frame unwinds. Destroy the
    /// executor only while whatever those coroutines touch is still alive
    /// (in particular, not from a late static destructor).
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

    /// True only once an owner is bound and the caller is that thread.
    /// Unlike is_owner_thread(), an executor nobody has pumped yet answers
    /// false: no thread can be assumed to run its queue.
    [[nodiscard]] bool owns_current_thread() const noexcept {
        return owner_.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        std::lock_guard lk(m_);
        return queue_.size();
    }

    /// Drop all pending callables without running them. Owner-thread-only.
    ///
    /// A queued `schedule_on` hop is not dropped silently: destroying it
    /// resumes its coroutine here, on the owner thread, and the `co_await`
    /// throws OperationCancelled, so the frame unwinds instead of leaking.
    void clear() {
        bind_owner_();
        std::deque<std::function<void()>> retired;
        {
            std::lock_guard lk(m_);
            retired.swap(queue_);
        }
        // Destroy user captures (and resume discarded hops) after unlocking:
        // their destructors may post.
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

namespace detail {

/// Whether `exec` runs its queue on a single owner thread (Caps::MainThread).
/// Only such an executor can be blocked by a thread waiting for its own
/// queue, and only it can tell whether the caller is that thread.
[[nodiscard]] inline bool thread_affine(const IExecutor& exec) noexcept {
    return aria::has_caps(exec, aria::SchedulerCaps::MainThread);
}

/// True when the calling thread is the one `exec` runs its work on. A
/// MainThreadExecutor nobody has pumped yet answers false; executors without
/// thread affinity always answer false.
[[nodiscard]] inline bool on_executor_thread(const IExecutor& exec) noexcept {
    if (const auto* main = dynamic_cast<const MainThreadExecutor*>(&exec)) {
        return main->owns_current_thread();
    }
    return thread_affine(exec) && exec.is_main_thread();
}

/// Gives a single-use resumption the copy semantics `std::function` needs.
///
/// Moving transfers the resumption. Copying shares it, allocating a shared
/// block only then, so the usual move-only queueing stays allocation-free.
/// Whichever copy is invoked first resumes the coroutine; when the last copy
/// is destroyed without any having run, the hop is discarded (the coroutine
/// resumes as cancelled). Copying one queued callable concurrently from
/// several threads is not supported.
///
/// `Core` is a move-only, single-shot pair of `run()` and `discard()`;
/// a moved-from or spent core does nothing.
template<typename Core>
class SharedOnCopy {
public:
    explicit SharedOnCopy(Core core) noexcept : core_(std::move(core)) {}
    SharedOnCopy(SharedOnCopy&& other) noexcept = default;
    SharedOnCopy(const SharedOnCopy& other) : shared_(share_of(other)) {}
    SharedOnCopy& operator=(SharedOnCopy&& other) noexcept {
        if (this != &other) {
            release();
            core_ = std::move(other.core_);
            shared_ = std::move(other.shared_);
        }
        return *this;
    }
    SharedOnCopy& operator=(const SharedOnCopy& other) {
        if (this != &other) {
            SharedOnCopy copy{other};
            *this = std::move(copy);
        }
        return *this;
    }
    ~SharedOnCopy() { release(); }

    void operator()() {
        if (!shared_) {
            core_.run();
            return;
        }
        if (!shared_->taken.exchange(true, std::memory_order_acq_rel)) {
            shared_->core.run();
        }
    }

private:
    struct Shared {
        explicit Shared(Core&& value) noexcept : core(std::move(value)) {}
        Shared(const Shared&) = delete;
        Shared& operator=(const Shared&) = delete;
        Shared(Shared&&) = delete;
        Shared& operator=(Shared&&) = delete;
        // The last copy is gone. If none of them ran, the hop was discarded.
        ~Shared() {
            if (!taken.load(std::memory_order_acquire)) {
                core.discard();
            }
        }
        Core core;
        std::atomic<bool> taken{false};
    };

    // Static indirection: the lazy share() only writes mutable bookkeeping,
    // which bugprone-copy-constructor-mutates-argument cannot see through.
    static const std::shared_ptr<Shared>& share_of(const SharedOnCopy& other) {
        return other.share();
    }

    const std::shared_ptr<Shared>& share() const {
        if (!shared_) {
            // make_shared allocates before it moves the core, so a failure
            // leaves this resumption intact.
            shared_ = std::make_shared<Shared>(std::move(core_));
        }
        return shared_;
    }

    void release() noexcept {
        if (shared_) {
            shared_.reset();  // the last owner's ~Shared decides
            return;
        }
        core_.discard();
    }

    mutable Core core_;
    mutable std::shared_ptr<Shared> shared_;
};

/// Core of a hop that observes no cancellation. Running it resumes the
/// coroutine. Discarding it (the executor is closed, or destroyed with work
/// queued) resumes the coroutine with `cancelled` set, so the frame and
/// everything it owns unwind instead of staying suspended forever.
class PlainHop {
public:
    PlainHop() noexcept = default;  // spent
    PlainHop(std::coroutine_handle<> handle, bool* cancelled) noexcept
        : handle_(handle), cancelled_(cancelled) {}
    PlainHop(PlainHop&& other) noexcept
        : handle_(std::exchange(other.handle_, {})), cancelled_(other.cancelled_) {}
    PlainHop& operator=(PlainHop&& other) noexcept {
        // Only assigned once spent (SharedOnCopy releases first).
        handle_ = std::exchange(other.handle_, {});
        cancelled_ = other.cancelled_;
        return *this;
    }
    PlainHop(const PlainHop&) = delete;
    PlainHop& operator=(const PlainHop&) = delete;
    ~PlainHop() = default;

    void run() {
        if (auto handle = std::exchange(handle_, {})) {
            handle.resume();
        }
    }

    void discard() noexcept {
        auto handle = std::exchange(handle_, {});
        if (!handle) {
            return;
        }
        *cancelled_ = true;  // the frame is alive until this resume
        try {
            handle.resume();
        } catch (...) {
            // Only a foreign promise type can throw out of resume().
            ::aria::report_callback_failure("executor.discarded_hop", std::current_exception());
        }
    }

private:
    std::coroutine_handle<> handle_;
    bool* cancelled_ = nullptr;
};

using PlainResumption = SharedOnCopy<PlainHop>;

/// Shared state of a hop on a thread-affine executor that also observes
/// cancellation. Two parties race to resume the coroutine: the executor
/// (running or discarding the queued resumption) and cancellation on the
/// executor's own thread. Exactly one wins, and the continuation always runs
/// on the executor's thread, except when the executor discards the hop.
///
/// While await_suspend is still running ("preparing"), the executor running
/// the resumption on the preparing thread (an executor that runs work in
/// place) never resumes on that stack; await_suspend declines to suspend
/// instead. A remote executor may resume at once: after posting,
/// await_suspend only touches this shared state, never the coroutine frame.
///
/// Cancellation never resumes a preparing hop, nor one whose executor runs
/// on another thread: such a hop parks on the token (see AffineWaiter) and
/// throws OperationCancelled when its executor resumes it.
struct CancellableHop final : AffineWaiter {
    enum class Phase : std::uint8_t {
        Preparing,
        Suspended,
        Resumed,          // by the executor
        Cancelled,        // by cancellation or a discarding executor
        InlineResumed,    // continue in place
        InlineCancelled,  // continue in place, then throw
    };

    CancellableHop(std::coroutine_handle<> coroutine, IExecutor& target,
                   std::weak_ptr<CancellationState> token) noexcept
        : handle(coroutine), exec(&target), token_state(std::move(token)) {}

    std::atomic<Phase> phase{Phase::Preparing};
    std::coroutine_handle<> handle;
    IExecutor* exec;
    std::weak_ptr<CancellationState> token_state;
    std::thread::id preparer = std::this_thread::get_id();

    /// The executor runs (`cancel` false) or discards (`cancel` true) the
    /// queued resumption. Returns true when the caller must resume now.
    bool claim_executor(bool cancel) noexcept {
        Phase observed = phase.load(std::memory_order_acquire);
        for (;;) {
            Phase next = Phase::Resumed;
            bool resume_now = true;
            if (observed == Phase::Preparing && std::this_thread::get_id() == preparer) {
                next = cancel ? Phase::InlineCancelled : Phase::InlineResumed;
                resume_now = false;
            } else if (observed == Phase::Preparing || observed == Phase::Suspended) {
                next = cancel ? Phase::Cancelled : Phase::Resumed;
            } else {
                return false;
            }
            if (phase.compare_exchange_weak(observed, next,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                return resume_now;
            }
        }
    }

    /// Early cancellation on the executor's thread takes only a hop that is
    /// fully suspended in the queue. Returns true when it must resume now.
    bool claim_cancel() noexcept {
        Phase expected = Phase::Suspended;
        return phase.compare_exchange_strong(expected, Phase::Cancelled,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
    }

    /// await_suspend's result: true keeps the coroutine suspended (or it was
    /// already resumed elsewhere), false continues it in place.
    bool commit() noexcept {
        Phase observed = Phase::Preparing;
        if (phase.compare_exchange_strong(observed, Phase::Suspended,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return true;
        }
        return observed == Phase::Resumed || observed == Phase::Cancelled;
    }

    [[nodiscard]] bool pending() const noexcept {
        const Phase observed = phase.load(std::memory_order_acquire);
        return observed == Phase::Preparing || observed == Phase::Suspended;
    }

    [[nodiscard]] bool claimed_as_cancelled() const noexcept {
        const Phase observed = phase.load(std::memory_order_acquire);
        return observed == Phase::Cancelled || observed == Phase::InlineCancelled;
    }

    void park(const std::shared_ptr<CancellableHop>& self) const {
        if (const auto state = token_state.lock()) {
            state->park(self);
        }
    }

    /// The token's cancellation callback. On the executor's own thread a
    /// queued hop resumes at once, so a thread tearing down its scope never
    /// waits for its own queue. Anywhere else the hop parks.
    static void on_cancelled(const std::shared_ptr<CancellableHop>& hop) {
        if (!hop->pending()) {
            return;
        }
        if (on_executor_thread(*hop->exec) && hop->claim_cancel()) {
            schedule_deferred_resume(hop->handle);
            return;
        }
        hop->park(hop);
    }

    bool resume_if_on_target_thread() noexcept override {
        const Phase observed = phase.load(std::memory_order_acquire);
        if (observed == Phase::Preparing) {
            return false;  // look again on the next pass
        }
        if (observed != Phase::Suspended) {
            return true;
        }
        if (!on_executor_thread(*exec)) {
            return false;
        }
        if (claim_cancel()) {
            schedule_deferred_resume(handle);
        }
        return true;
    }
};

/// The executor's side of a cancellable hop.
class QueuedHop {
public:
    QueuedHop() noexcept = default;  // spent
    explicit QueuedHop(std::shared_ptr<CancellableHop> hop) noexcept : hop_(std::move(hop)) {}
    QueuedHop(QueuedHop&& other) noexcept = default;
    QueuedHop& operator=(QueuedHop&& other) noexcept = default;
    QueuedHop(const QueuedHop&) = delete;
    QueuedHop& operator=(const QueuedHop&) = delete;
    ~QueuedHop() = default;

    void run() {
        auto hop = std::exchange(hop_, nullptr);
        if (hop && hop->claim_executor(false)) {
            hop->handle.resume();
        }
    }

    void discard() noexcept {
        auto hop = std::exchange(hop_, nullptr);
        if (!hop || !hop->claim_executor(true)) {
            return;
        }
        try {
            hop->handle.resume();
        } catch (...) {
            // Only a foreign promise type can throw out of resume().
            ::aria::report_callback_failure("executor.discarded_hop", std::current_exception());
        }
    }

private:
    std::shared_ptr<CancellableHop> hop_;
};

using CancellableResumption = SharedOnCopy<QueuedHop>;

class ScheduleAwaiter {
public:
    ScheduleAwaiter(IExecutor& exec, std::optional<CancellationToken> token) noexcept
        : exec_(exec), explicit_token_(std::move(token)) {}

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): awaiter protocol
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        return suspend(h);
    }

    /// The suspension logic itself, for wrapping awaiters that report the
    /// hop's outcome differently.
    template<typename Promise>
    bool suspend(std::coroutine_handle<Promise> h) {
        token_ = explicit_token_ ? &*explicit_token_ : ambient_cancellation_of(h);
        if (token_ != nullptr && !token_->can_be_cancelled()) {
            token_ = nullptr;
        }
        if (dynamic_cast<InlineExecutor*>(&exec_) != nullptr) {
            // post() would resume the coroutine nested inside this call, on
            // this very thread; continuing in place is the same hop without
            // growing the stack. await_resume still observes the token.
            return false;
        }
        if (token_ != nullptr && token_->is_cancelled() && on_executor_thread(exec_)) {
            return false;  // already on the target thread: throw in place
        }
        if (token_ == nullptr || !thread_affine(exec_)) {
            // No early resumption is possible: the executor runs the hop and
            // await_resume observes the token there. The resumption may run
            // (or be discarded) before post() returns, on this or another
            // thread, so nothing here is touched after it.
            try {
                exec_.post(PlainResumption{PlainHop{h, &plain_cancelled_}});
            } catch (...) {
                // The failed post destroyed the resumption, which already
                // resumed the coroutine as cancelled; propagating would
                // resume it a second time.
                ::aria::report_callback_failure("executor.post", std::current_exception());
            }
            return true;
        }
        auto hop = std::make_shared<CancellableHop>(
            h, exec_, CancellationTokenAccess::state(*token_));
        shared_ = hop;
        if (token_->is_cancelled()) {
            // Its executor's thread may be blocked joining this scope.
            hop->park(hop);
        } else {
            cancellation_ = CancellationCallback(*token_, [hop] {
                CancellableHop::on_cancelled(hop);
            });
        }
        // From here on only `hop` is used: a remote executor may resume and
        // destroy the frame as soon as the resumption is queued.
        exec_.post(CancellableResumption{QueuedHop{hop}});
        return hop->commit();
    }

    void await_resume() const {
        if (resumed_cancelled()) {
            throw OperationCancelled{};
        }
    }

    /// True when the hop ended in cancellation: discarded by the executor,
    /// or resumed while its token is cancelled.
    [[nodiscard]] bool resumed_cancelled() const noexcept {
        const bool discarded = shared_ ? shared_->claimed_as_cancelled() : plain_cancelled_;
        return discarded || (token_ != nullptr && token_->is_cancelled());
    }

private:
    IExecutor& exec_;
    std::optional<CancellationToken> explicit_token_;
    const CancellationToken* token_ = nullptr;
    bool plain_cancelled_ = false;
    std::shared_ptr<CancellableHop> shared_;
    CancellationCallback cancellation_;
};

/// `schedule_on` that reports cancellation instead of throwing it.
///
/// Composes `ScheduleAwaiter` rather than deriving from it: `await_resume`
/// must keep its exact name for the awaiter protocol, and a same-named member
/// with different behaviour would shadow the base's.
class TryScheduleAwaiter {
public:
    TryScheduleAwaiter(IExecutor& exec, std::optional<CancellationToken> token) noexcept
        : inner_{exec, std::move(token)} {}

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static): awaiter protocol
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) {
        return inner_.suspend(h);
    }

    /// False exactly where `schedule_on` would throw OperationCancelled.
    [[nodiscard]] bool await_resume() const noexcept { return !inner_.resumed_cancelled(); }

private:
    ScheduleAwaiter inner_;
};

}  // namespace detail

/// Resume the awaiting coroutine on `exec`.
///
/// Inside a CoroutineScope the hop observes the scope's cancellation: once
/// the scope is cancelled, `co_await` throws OperationCancelled. The
/// continuation, including that throw, still runs on `exec`'s thread:
///
///   * Already cancelled: on `exec`'s own thread the hop throws in place;
///     anywhere else it is queued as usual and throws when `exec` runs it.
///   * Cancelled while queued on a thread-affine executor (Caps::MainThread)
///     from that executor's own thread: the coroutine resumes at once and
///     throws, so scope teardown on that thread never waits for the queue it
///     is blocking. Cancelled from another thread: the hop stays queued, and
///     `CoroutineScope::cancel_and_join` on the executor's thread resumes it.
///
/// Outside a scope the hop does not observe cancellation.
///
/// If the executor discards the resumption (it is closed or destroyed), the
/// coroutine resumes on the discarding thread and `co_await` throws
/// OperationCancelled, instead of leaving its frame suspended forever.
inline detail::ScheduleAwaiter schedule_on(IExecutor& exec) {
    return detail::ScheduleAwaiter{exec, std::nullopt};
}

/// As above, observing `token` instead of the scope's cancellation. Pass
/// `CancellationToken::none()` for a hop that must happen even after
/// cancellation (cleanup that publishes final state on the UI executor).
inline detail::ScheduleAwaiter schedule_on(IExecutor& exec, CancellationToken token) {
    return detail::ScheduleAwaiter{exec, std::move(token)};
}

/// Same hop as `schedule_on`, on the same thread, but `co_await` yields
/// false instead of throwing OperationCancelled. For cleanup paths that must
/// not throw (AsyncCommand reports such a hop as a Cancelled result).
inline detail::TryScheduleAwaiter try_schedule_on(IExecutor& exec) {
    return detail::TryScheduleAwaiter{exec, std::nullopt};
}

/// As above, observing `token` instead of the scope's cancellation.
inline detail::TryScheduleAwaiter try_schedule_on(IExecutor& exec, CancellationToken token) {
    return detail::TryScheduleAwaiter{exec, std::move(token)};
}

}  // namespace aria::async
