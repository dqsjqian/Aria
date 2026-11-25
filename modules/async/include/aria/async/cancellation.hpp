#pragma once

// Cooperative cancellation — inspired by Kotlin Coroutines `Job` / Swift's
// `Task.checkCancellation()` / std::stop_token.
//
//   CancellationSource src;
//   auto token = src.token();
//
//   Task<int> work(CancellationToken tok) {
//       co_await schedule_on(pool);
//       tok.throw_if_cancelled();          // probe at safe points
//       co_return heavy_computation();
//   }
//
//   src.cancel();   // any work() that probes will throw OperationCancelled
//
// Tokens are thread-safe and copyable; cancellation propagates to all copies.

#include "aria/callback_boundary.hpp"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace aria::async {

class OperationCancelled : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "operation was cancelled";
    }
};

namespace detail {

/// An executor hop that only its executor's own thread may resume early.
///
/// A hop queued on a thread-affine executor (a UI thread, say) resumes on
/// that thread when its cancellation is requested there. Cancellation that
/// arrives on any other thread must not resume it in place, so the hop parks
/// itself on the token instead: the executor either runs it normally later,
/// or the executor's thread, blocked in `CoroutineScope::cancel_and_join` and
/// therefore unable to run its queue, resumes it from the parked list.
struct AffineWaiter {
    AffineWaiter() = default;
    AffineWaiter(const AffineWaiter&) = delete;
    AffineWaiter& operator=(const AffineWaiter&) = delete;
    AffineWaiter(AffineWaiter&&) = delete;
    AffineWaiter& operator=(AffineWaiter&&) = delete;
    virtual ~AffineWaiter() = default;

    /// Resume the hop as cancelled if the calling thread is its executor's
    /// thread and it is still queued. Returns true once the entry needs no
    /// further attention (resumed here, or already resumed elsewhere).
    virtual bool resume_if_on_target_thread() noexcept = 0;
};

/// Shared cancellation state. Callbacks are identified so a scoped
/// registration (`CancellationCallback`) can withdraw itself; without
/// that, every bounded wait on a long-lived token would leave its callback
/// behind until the token is finally cancelled.
struct CancellationState {
    using CallbackId = std::uint64_t;
    // Keyed by registration order: withdrawal is O(log n) even with many
    // concurrent waits, and cancel() still fires in registration order.
    using Callbacks = std::map<CallbackId, std::function<void()>>;

    std::atomic<bool> cancelled{false};
    std::mutex m;
    std::condition_variable callback_done;
    Callbacks callbacks;  // registered, not yet taken by cancel()
    Callbacks firing;     // taken by cancel(), not yet started
    CallbackId next_id = 0;
    CallbackId running = 0;        // callback executing now, 0 when none
    std::thread::id running_thread{};
    // Hops waiting for their executor's thread; see AffineWaiter.
    std::vector<std::weak_ptr<AffineWaiter>> parked;
    std::size_t parked_compact_at = 16;

    /// Registers `fn` unless cancellation already happened. Returns 0 when
    /// the caller must invoke `fn` itself (already cancelled).
    CallbackId add(std::function<void()>& fn) {
        const std::scoped_lock lock(m);
        if (cancelled.load(std::memory_order_acquire)) {
            return 0;
        }
        const auto id = ++next_id;
        callbacks.emplace_hint(callbacks.end(), id, std::move(fn));
        return id;
    }

    /// Withdraws registration `id`. If cancel() is executing that callback on
    /// another thread, waits for it to return (std::stop_callback semantics):
    /// afterwards nothing the callback captured is used. A callback that
    /// withdraws itself on its own thread does not wait.
    void remove(CallbackId id) noexcept {
        std::function<void()> withdrawn;  // destroyed after the lock below
        std::unique_lock lk(m);
        // A callback that has not started is withdrawn. One that is no
        // longer listed is running or has run; that is handled below.
        for (Callbacks* entries : {&callbacks, &firing}) {
            if (const auto found = entries->find(id); found != entries->end()) {
                withdrawn.swap(found->second);
                entries->erase(found);
                return;
            }
        }
        if (running == id && running_thread != std::this_thread::get_id()) {
            callback_done.wait(lk, [&] { return running != id; });
        }
    }

    /// Runs every registered callback once, in registration order, without
    /// holding the mutex. Each entry is claimed under the mutex so a
    /// concurrent remove() either withdraws it first or waits for it.
    void fire() {
        {
            const std::scoped_lock lock(m);
            firing.swap(callbacks);
        }
        for (;;) {
            std::function<void()> fn;
            {
                const std::scoped_lock lock(m);
                if (firing.empty()) {
                    break;
                }
                const auto next = firing.begin();
                fn.swap(next->second);
                running = next->first;
                running_thread = std::this_thread::get_id();
                firing.erase(next);
            }
            try {
                fn();
            } catch (...) {
                // Same policy as before scoped registrations: a throwing
                // callback must not stop the broadcast to the others.
                ::aria::report_callback_failure("cancellation.callback", std::current_exception());
            }
            fn = nullptr;
            {
                const std::scoped_lock lock(m);
                running = 0;
                running_thread = {};
            }
            callback_done.notify_all();
        }
    }

    /// Records a hop that only its executor's thread may resume early.
    /// Entries of hops that resumed normally expire and are compacted here.
    void park(std::weak_ptr<AffineWaiter> waiter) {
        const std::scoped_lock lock(m);
        if (parked.size() >= parked_compact_at) {
            std::erase_if(parked, [](const std::weak_ptr<AffineWaiter>& entry) {
                return entry.expired();
            });
            parked_compact_at = (std::max)(std::size_t{16}, parked.size() * 2);
        }
        parked.push_back(std::move(waiter));
    }

    /// Resumes, as cancelled, every parked hop whose executor runs on the
    /// calling thread. Defined after DrainScope.
    void resume_parked_here() noexcept;
};

// ---------------------------------------------------------------------
// Deferred-resume queue
//
// Cancellation callbacks must not call `coroutine_handle::resume()` from
// inside the cancellation broadcast loop. Doing so resumes the parked
// coroutine on a stack like
//
//     cancel()
//      └─ for c in cbs
//          └─ std::function::operator()
//              └─ cb lambda
//                  └─ h.resume()  <- coroutine body runs here
//
// MSVC release builds were observed to silently bypass coroutine-internal
// `try/catch` handlers when an exception unwinds out of `h.resume()` on
// this stack shape; MinGW UCRT64 went further and SIGSEGV'd inside the
// exception unwind. Both are symptoms of the SEH / DWARF personality
// routine getting confused about which try/catch ranges are live when
// the throwing PC sits inside a coroutine frame whose execution was
// re-entered through an `std::function::operator()` indirection layered
// on top of a `std::vector` iterator.
//
// The fix is to *defer* the resume: callbacks push their handle into a
// thread-local pending list, and the topmost `cancel()` on the thread
// drains that list once all callbacks have returned. Resumption then
// runs on the cancel() function's own stack frame, with no
// `std::function` indirection in between, and exception unwind out of
// the resumed coroutine sees a vanilla call stack that every supported
// toolchain handles correctly.
//
// The design is reentrancy-safe: a callback may itself trigger another
// `cancel()`. The inner cancel() observes that drain ownership is
// already taken by an outer cancel() and refrains from draining; the
// outer (topmost) cancel() picks up the union of pending handles and
// drains them in FIFO order.
//
// Cross-DLL note: `deferred_resume_context()` is `inline` and uses a
// function-local `thread_local`. Under C++17 inline-variable rules the
// per-thread storage is unique per (thread, DLL) pair on Windows. As
// long as a single `cancel()` call is contained inside one DLL
// boundary the deferred-resume protocol is honoured exactly. If a cb
// crosses into another DLL whose awaiter then calls
// `schedule_deferred_resume`, that call lands in the *other* DLL's
// per-thread context; if no outer cancel() is active there, it falls
// back to the inline-drain path documented below — still safe, just
// not deferred. Keeping a single CancellationSource within one
// translation-unit boundary is the recommended pattern.
struct DeferredResumeContext {
    std::vector<std::coroutine_handle<>> pending;
    bool draining = false;
};

inline DeferredResumeContext& deferred_resume_context() noexcept {
    thread_local DeferredResumeContext ctx;
    return ctx;
}

inline void schedule_deferred_resume(std::coroutine_handle<> h) noexcept {
    auto& ctx = deferred_resume_context();
    if (ctx.draining) {
        // Inside the broadcast loop of a cancel() higher up the stack —
        // queue the handle; that cancel() will drain it on its way out.
        ctx.pending.push_back(h);
        return;
    }
    // Defensive fallback: a deferred resume was scheduled outside any
    // active cancel() broadcast. Drain it inline (still on the caller's
    // own frame, not nested inside `std::function::operator()`).
    ctx.draining = true;
    ctx.pending.push_back(h);
    // Drain in FIFO order; new entries pushed during a resume are
    // appended to the same vector and processed before we exit the loop.
    // A resume can throw (foreign promises may have non-noexcept
    // unhandled_exception); we are noexcept ourselves and must not let
    // the exception escape — if it did, the caller (a cancellation
    // callback path) would call std::terminate. Swallow it after
    // restoring `draining = false` so the thread-local context is left
    // in a clean state for the next cancel().
    for (std::size_t i = 0; i < ctx.pending.size(); ++i) {
        auto coro = ctx.pending[i];
        if (!coro) continue;
        try {
            coro.resume();
        } catch (...) {
            // Drop — deferred-resume is a transport, not the right place
            // to attribute exceptions to a specific awaiter.
        }
    }
    ctx.pending.clear();
    ctx.draining = false;
}

// RAII helper used by CancellationSource::cancel() to make itself the
// owner of the drain phase if no outer cancel() is already active.
class DrainScope {
public:
    DrainScope() noexcept {
        auto& ctx = deferred_resume_context();
        is_owner_ = !ctx.draining;
        if (is_owner_) {
            ctx.draining = true;
        }
    }
    ~DrainScope() noexcept {
        if (!is_owner_) return;
        auto& ctx = deferred_resume_context();
        // Drain in FIFO order. Resuming a coroutine may push new
        // handles via further deferred resumes nested under it; the
        // index-based loop naturally picks those up.
        //
        // We are noexcept; a foreign coroutine handle whose
        // unhandled_exception is not noexcept could in theory throw
        // out of resume(). Swallow it so the thread-local context is
        // restored cleanly for the next cancel(); the exception is
        // dropped because the deferred-resume queue is just a
        // transport — not the right place to attribute failures to a
        // specific awaiter.
        for (std::size_t i = 0; i < ctx.pending.size(); ++i) {
            auto coro = ctx.pending[i];
            if (!coro) continue;
            try {
                coro.resume();
            } catch (...) {
                // intentional drop
            }
        }
        ctx.pending.clear();
        ctx.draining = false;
    }
    DrainScope(const DrainScope&) = delete;
    DrainScope& operator=(const DrainScope&) = delete;
    [[nodiscard]] bool is_owner() const noexcept { return is_owner_; }
private:
    bool is_owner_ = false;
};

inline void CancellationState::resume_parked_here() noexcept {
    // Resumptions are deferred to the end of `drain`: they run after the
    // lock is released, outside the loop below and its lambda.
    const DrainScope drain;
    std::vector<std::weak_ptr<AffineWaiter>> pending;
    {
        const std::scoped_lock lock(m);
        pending.swap(parked);
    }
    std::erase_if(pending, [](const std::weak_ptr<AffineWaiter>& entry) {
        const auto waiter = entry.lock();
        return !waiter || waiter->resume_if_on_target_thread();
    });
    if (pending.empty()) {
        return;
    }
    try {
        const std::scoped_lock lock(m);
        parked.insert(parked.end(), std::make_move_iterator(pending.begin()),
                      std::make_move_iterator(pending.end()));
    } catch (...) {
        // Only the early resumption is lost; each executor still runs its hop.
        ::aria::report_callback_failure("cancellation.park", std::current_exception());
    }
}
}  // namespace detail

class CancellationCallback;
class CancellationToken;

namespace detail {
struct CancellationTokenAccess;
}  // namespace detail

class CancellationToken {
public:
    CancellationToken() = default;
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> s)
        : state_(std::move(s)) {}

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    /// True when a source exists, i.e. cancellation can ever be observed.
    /// `none()` and default-constructed tokens return false.
    [[nodiscard]] bool can_be_cancelled() const noexcept { return state_ != nullptr; }

    /// Throw OperationCancelled if cancelled. Call at safe await points.
    void throw_if_cancelled() const {
        if (is_cancelled()) throw OperationCancelled{};
    }

    /// Register a callback fired (synchronously) when source is cancelled.
    /// If already cancelled, invoked immediately.
    ///
    /// The registration lives until cancellation: there is no way to
    /// withdraw it. Bounded waits on long-lived tokens must use
    /// `CancellationCallback` instead, or each wait leaks its callback.
    void on_cancel(std::function<void()> cb) {
        // A synchronous callback may destroy this token and the source. Keep
        // the state alive and never call user code while its mutex is held.
        const auto state = state_;
        if (!state || !cb) {
            return;
        }
        if (state->add(cb) == 0) {
            cb();
        }
    }

    /// Always-cancellable empty token (useful as a default).
    [[nodiscard]] static CancellationToken none() noexcept { return {}; }

private:
    friend class CancellationCallback;
    friend struct detail::CancellationTokenAccess;
    std::shared_ptr<detail::CancellationState> state_;
};

namespace detail {
/// Framework access to a token's shared state (executor hops and scopes).
struct CancellationTokenAccess {
    [[nodiscard]] static const std::shared_ptr<CancellationState>& state(
        const CancellationToken& token) noexcept {
        return token.state_;
    }
};
}  // namespace detail

/// Scoped cancellation registration, the counterpart of std::stop_callback.
///
/// Constructing it registers `fn` on the token (or invokes it immediately
/// when the token is already cancelled); destroying or `reset()`-ing it
/// withdraws the registration. If the callback is running on another thread
/// at that moment, withdrawal waits for it to return, so state captured by
/// the callback may be released right after. A callback must not withdraw a
/// registration whose callback is blocked on the withdrawing thread.
///
/// Because withdrawal can block, never destroy or reset() a registration
/// while holding a lock that its callback acquires: the cancelling thread
/// waits for the lock inside the callback while the withdrawing thread waits
/// for the callback, and neither returns. Keep callbacks short; if one must
/// take a lock, release that lock before withdrawing (std::stop_callback has
/// the same rule).
class [[nodiscard]] CancellationCallback {
public:
    CancellationCallback() noexcept = default;

    template<typename Fn>
        requires std::invocable<std::decay_t<Fn>&> &&
                 (!std::same_as<std::decay_t<Fn>, CancellationCallback>)
    CancellationCallback(const CancellationToken& token, Fn&& fn) {
        const auto& state = token.state_;
        if (!state) {
            return;
        }
        std::function<void()> callback{std::forward<Fn>(fn)};
        const auto id = state->add(callback);
        if (id == 0) {
            callback();
            return;
        }
        state_ = state;
        id_ = id;
    }

    ~CancellationCallback() { reset(); }

    CancellationCallback(const CancellationCallback&) = delete;
    CancellationCallback& operator=(const CancellationCallback&) = delete;
    CancellationCallback(CancellationCallback&& other) noexcept
        : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}
    CancellationCallback& operator=(CancellationCallback&& other) noexcept {
        if (this != &other) {
            reset();
            state_ = std::move(other.state_);
            id_ = std::exchange(other.id_, 0);
        }
        return *this;
    }

    /// Withdraw now; idempotent.
    void reset() noexcept {
        if (auto state = std::exchange(state_, nullptr)) {
            state->remove(std::exchange(id_, 0));
        }
    }

    /// True while registered and not yet taken by a cancellation.
    [[nodiscard]] bool registered() const noexcept { return state_ != nullptr; }

private:
    std::shared_ptr<detail::CancellationState> state_;
    detail::CancellationState::CallbackId id_ = 0;
};

class CancellationSource {
public:
    CancellationSource() : state_(std::make_shared<detail::CancellationState>()) {}

    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken{state_};
    }

    /// Trigger cancellation. All currently registered callbacks fire
    /// synchronously on the calling thread; any later observers see
    /// is_cancelled() == true.
    ///
    /// Coroutine resumes triggered by callbacks are *deferred*: the
    /// callback enqueues the coroutine handle, and this function drains
    /// the queue once every callback has returned. That keeps the stack
    /// shape free of `std::function::operator()` indirection at the
    /// moment a resumed coroutine throws — see the comment on
    /// `detail::DeferredResumeContext` for the full rationale.
    ///
    /// Safe to call on a moved-from CancellationSource (state_ may
    /// be nullptr). The destructor relies on this guard.
    void cancel() {
        if (!state_) return;
        bool expected = false;
        if (!state_->cancelled.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;  // already cancelled
        }
        // Become the drain owner if no outer cancel() is already in the
        // broadcast phase on this thread. The destructor of `drain`
        // will run pending coroutine resumes on this function's frame
        // — not nested inside any `std::function` invocation.
        detail::DrainScope drain;
        // A callback may destroy this source; the broadcast owns the state.
        auto state = state_;
        state->fire();
        // ~DrainScope here resumes any deferred coroutine handles.
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    /// Auto-cancel on destruction — perfect for ViewModelScope.
    ~CancellationSource() { cancel(); }

    CancellationSource(const CancellationSource&) = delete;
    CancellationSource& operator=(const CancellationSource&) = delete;
    CancellationSource(CancellationSource&&) = default;
    CancellationSource& operator=(CancellationSource&& other) noexcept {
        if (this != &other) {
            // Release the previous source just as destruction would: its
            // tokens may still own the state and have parked waiters.
            cancel();
            state_ = std::move(other.state_);
        }
        return *this;
    }

private:
    std::shared_ptr<detail::CancellationState> state_;
};

}  // namespace aria::async
