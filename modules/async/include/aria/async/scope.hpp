#pragma once

// CoroutineScope — structured concurrency primitive, modelled after
// Kotlin's `CoroutineScope` / `viewModelScope` and Swift's `TaskGroup`.
//
// Contract (the things a "global-class C++ framework" must guarantee):
//
//   * Every coroutine launched into a scope is *owned* by the scope.
//   * `cancel()` is non-blocking: it requests cancellation, no more.
//   * `cancel_and_join()` (sync) and `co_await join()` (async) wait until
//     every in-flight coroutine has exited. Tests, ViewModel teardown,
//     app shutdown — all use one of these to drain.
//   * The destructor MUST NOT let a launched coroutine outlive the scope.
//     It therefore performs `cancel_and_join()` with a bounded wait
//     (5 s by default). If the wait times out (a coroutine is stuck on
//     a non-cancellable await), the leak is reported through the async
//     error sink — we never block process exit indefinitely.
//   * Unhandled exceptions on the detached path do NOT vanish: any
//     non-`OperationCancelled` exception is forwarded to the async
//     error sink (see <aria/async/async_error_sink.hpp>).
//   * Scopes nest: a child scope constructed with a parent token is
//     automatically cancelled when the parent cancels.
//
// Backward-compatible API: existing `scope.launch(factory)` and
// `scope.launch_simple(task)` calls keep working unchanged.
//
//   CoroutineScope scope;
//   scope.launch([](CancellationToken t) -> Task<void> {
//       while (!t.is_cancelled()) {
//           co_await schedule_on(pool);
//           do_work();
//       }
//   });
//   co_await scope.join();   // wait for everyone to drain (cooperatively)
//
//   // Or, in a synchronous teardown path:
//   scope.cancel_and_join();

#include "aria/async/async_error_sink.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/task.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace aria::async {

namespace detail {

/// Shared accounting block — owned jointly by the scope and every wrapper
/// coroutine spawned through `launch*`. Allows wrappers to safely
/// decrement the in-flight counter even after the scope object itself
/// has been destroyed (which can happen if `cancel_and_join` times out
/// and the destructor returns while a stuck coroutine is still alive).
struct ScopeState {
    std::atomic<std::size_t> inflight{0};
    std::mutex mu;
    std::condition_variable cv;
    // Awaiters waiting for `inflight == 0` (registered by `join()`).
    std::vector<std::function<void()>> drain_waiters;
};

}  // namespace detail

class CoroutineScope : public std::enable_shared_from_this<CoroutineScope> {
public:
    /// Default scope — fully independent, no parent linkage.
    CoroutineScope() : state_(std::make_shared<detail::ScopeState>()) {}

    /// Child scope: cancelling `parent` cancels this scope as well.
    /// The child is otherwise independent (cancelling the child does
    /// NOT propagate up to the parent, matching Kotlin semantics).
    explicit CoroutineScope(CancellationToken parent)
        : state_(std::make_shared<detail::ScopeState>()) {
        register_parent_link_(std::move(parent));
    }

    ~CoroutineScope() {
        // Structured-concurrency invariant: no coroutine outlives the scope.
        //
        // `cancel_and_join` is itself `noexcept`, but the diagnostic path
        // it routes through (`report_async_error` -> user-installed sink)
        // can in principle throw `bad_alloc` from within the sink's
        // implementation. A destructor is implicitly `noexcept`, so any
        // escaping exception during stack unwinding would call
        // `std::terminate`. Wrap defensively.
        try {
            cancel_and_join();
        } catch (...) {
            // Last line of defence — an exception from the async error
            // sink is itself a (non-fatal) async error; we can't report
            // it back through the same channel without recursing, so
            // we silently drop. The leak (if any) was already accounted
            // for via `inflight_` reads.
        }
    }

    CoroutineScope(const CoroutineScope&) = delete;
    CoroutineScope& operator=(const CoroutineScope&) = delete;
    CoroutineScope(CoroutineScope&&) = delete;
    CoroutineScope& operator=(CoroutineScope&&) = delete;

    // ── Inspection ────────────────────────────────────────────────────

    [[nodiscard]] CancellationToken token() const noexcept { return src_.token(); }
    [[nodiscard]] bool is_cancelled() const noexcept { return src_.is_cancelled(); }

    /// Number of coroutines currently in flight (launched but not yet
    /// returned). Useful for tests and diagnostics.
    [[nodiscard]] std::size_t inflight_count() const noexcept {
        return state_->inflight.load(std::memory_order_acquire);
    }

    // ── Cancellation / join ───────────────────────────────────────────

    /// Request cancellation. Non-blocking: in-flight coroutines will
    /// observe the cancellation at their next probe / co_await.
    void cancel() noexcept {
        try { src_.cancel(); } catch (...) {}
    }

    /// Synchronously: cancel + wait for all in-flight coroutines to
    /// finish, with a bounded timeout (default 5 s). On timeout, a
    /// leak diagnostic is emitted via the async error sink and the
    /// function returns; the scope is left with a non-zero inflight
    /// count. Returns true if everyone drained, false on timeout.
    bool cancel_and_join(std::chrono::milliseconds timeout =
                             std::chrono::milliseconds{5000}) noexcept {
        cancel();
        std::unique_lock lk(state_->mu);
        const bool drained = state_->cv.wait_for(lk, timeout, [this] {
            return state_->inflight.load(std::memory_order_acquire) == 0;
        });
        if (!drained) {
            const auto leaked =
                state_->inflight.load(std::memory_order_acquire);
            // Hand back any joiner coroutines still parked in
            // `drain_waiters`. On the drained path `decrement_inflight_`
            // has already swapped the vector out and resumed them, but on
            // the timeout path nobody ever will: `inflight` never reaches
            // zero, so the last-one-out branch cannot fire. Leaving the
            // handles parked leaks a coroutine frame per joiner *on top of*
            // the tasks we are already reporting as leaked. Resume them so
            // the awaiting frames unwind and their destructors run.
            std::vector<std::function<void()>> stranded;
            stranded.swap(state_->drain_waiters);
            lk.unlock();
            for (auto& w : stranded) {
                try { w(); } catch (...) {}
            }
            report_async_error(
                std::string("CoroutineScope: dtor leaked ") +
                std::to_string(leaked) +
                " task(s) (cancellation observed but coroutines did not "
                "exit within the timeout)");
        }
        return drained;
    }

    /// Awaitable resumed when in-flight count reaches zero. The same
    /// type is used by `join()` and `join_existing()` — declared up here
    /// so we can give those methods explicit return types and avoid
    /// `auto` deduction ordering issues across templated contexts.
    struct JoinAwaiter {
        std::shared_ptr<detail::ScopeState> st;

        bool await_ready() const noexcept {
            return st->inflight.load(std::memory_order_acquire) == 0;
        }

        /// Returns `false` when the scope already drained between
        /// `await_ready()` and here, telling the compiler to resume the
        /// awaiting coroutine *in place* on its own frame.
        ///
        /// This must NOT call `h.resume()` on the current stack. Doing so
        /// makes `await_suspend` return into a frame the resumed coroutine
        /// may already have destroyed, and an exception unwinding out of
        /// the resumed body would cross `await_suspend`'s frame — both
        /// undefined behaviour. See contract (3) in the cancellation-resume
        /// commentary below: "`await_suspend` MUST NOT cause `h.resume()`
        /// to run on the current stack ... Returning `false` lets the
        /// compiler resume the caller in place."
        ///
        /// The drained check is repeated under the lock because
        /// `decrement_inflight_` swaps `drain_waiters` out under the same
        /// lock: either we observe zero and decline to suspend, or we
        /// enqueue and the last task out is guaranteed to see our entry.
        bool await_suspend(std::coroutine_handle<> h) {
            std::unique_lock lk(st->mu);
            if (st->inflight.load(std::memory_order_acquire) == 0) {
                return false;// resume in place, on the caller's frame
            }
            st->drain_waiters.emplace_back([h]() mutable { h.resume(); });
            return true;
        }

        void await_resume() const noexcept {}
    };

    /// Awaitable equivalent of `cancel_and_join()` — request cancellation
    /// then suspend the calling coroutine until in-flight count reaches
    /// zero. Does NOT have a timeout; intended for cooperative shutdown
    /// from inside another coroutine.
    [[nodiscard]] JoinAwaiter join() noexcept {
        cancel();
        return JoinAwaiter{state_};
    }

    /// Like `join()` but does NOT request cancellation first — it just
    /// waits for whatever is currently in flight to complete naturally.
    [[nodiscard]] JoinAwaiter join_existing() noexcept {
        return JoinAwaiter{state_};
    }

    // ── Launch ────────────────────────────────────────────────────────

    /// Launch a coroutine factory `Task<void> fn(CancellationToken)`.
    /// The returned task is wrapped, accounted for in `inflight_count()`,
    /// and any unhandled exception (other than `OperationCancelled`)
    /// is reported via the async error sink.
    ///
    /// IMPORTANT — lambda-captures lifetime contract:
    ///
    /// `factory` is almost always a lambda whose body is itself a
    /// coroutine (`[caps](CancellationToken tok) -> Task<void> { ... }`).
    /// A C++20 coroutine that lives inside a lambda body does NOT copy
    /// the lambda's captures into its own coroutine frame — instead, it
    /// stores `this` and reads captures through it. So if we let
    /// `factory` itself live only as long as the `launch()` call, every
    /// capture (e.g. `shared_ptr<atomic<bool>>` used to observe state
    /// from the test) is destroyed the moment `launch()` returns, while
    /// the user coroutine is still parked. Subsequent capture access
    /// from inside the body is undefined behaviour. The exact symptom
    /// observed in the wild was MSVC release builds producing
    /// `stopped->store(true)` writes that the main thread never read
    /// back, in the parent->child cancellation test.
    ///
    /// The fix is to host `factory` inside a tiny wrapper coroutine
    /// (`launch_owner_coro_`). Coroutine *parameters* (unlike lambda
    /// captures) are by-value-copied into the coroutine frame, so the
    /// wrapper frame owns `factory` for the entire lifetime of the user
    /// task. The user lambda's `this` pointer therefore remains valid
    /// until the user task completes.
    ///
    /// Cost note: each `launch()` therefore performs *two* coroutine
    /// frame heap allocations — one for `launch_owner_coro_`, one for
    /// the user task. Halo (heap-allocation-elision optimisation) is
    /// not eligible here because both frames outlive the call (they
    /// detach into `start_detached_()`). This is the unavoidable price
    /// of routing `factory` through a coroutine parameter slot; in
    /// every benchmarked workload it is dominated by the user task's
    /// own work and never becomes the bottleneck. If a caller
    /// genuinely needs the wrapper-free path (e.g. very high-frequency
    /// fire-and-forget launches with no captures), `launch_simple()`
    /// is the documented escape hatch.
    ///
    /// `Fn` is forwarded perfectly so that move-only callables
    /// (e.g. lambdas capturing `std::unique_ptr`) work, and lvalue
    /// callables are copied at the call site exactly once into the
    /// wrapper coroutine frame.
    template<typename Fn>
    void launch(Fn&& factory) {
        using FnDecay = std::decay_t<Fn>;
        spawn_tracked_(
            launch_owner_coro_<FnDecay>(std::forward<Fn>(factory), token()));
    }

    /// Convenience overload for a fully-formed `Task<void>` whose body
    /// already captures the cancellation token (or doesn't need one).
    ///
    /// Caller-owned capture lifetime contract:
    ///
    /// Unlike `launch()`, this overload does NOT host the originating
    /// callable inside an owner coroutine — it accepts the resulting
    /// `Task<void>` directly. If the caller produced that task by
    /// invoking a lambda whose body is itself a coroutine
    /// (`auto t = [caps]() -> Task<void> { ... }();`), the lambda is a
    /// temporary that dies at the end of the enclosing full-expression,
    /// while the coroutine frame still holds a `this` pointer back into
    /// it — captures become dangling. Prefer `launch(factory)` for the
    /// lambda-factory case; it owns the lambda for you.
    /// `launch_simple()` is the escape hatch for callers that have
    /// already arranged capture lifetimes themselves (e.g. by passing
    /// state through coroutine parameters, or by anchoring the lambda
    /// in a longer-lived storage).
    void launch_simple(Task<void> task) {
        spawn_tracked_(std::move(task));
    }

private:
    // ── Tracked spawn ─────────────────────────────────────────────────

    /// Decrement the in-flight counter and, if this was the last task
    /// out, fire any waiters registered via `join()` / `join_existing()`.
    /// Always notifies the cv so that timed waiters in
    /// `cancel_and_join()` observe progress promptly.
    ///
    /// Shared between `~InflightGuard` (the normal path) and
    /// `spawn_tracked_`'s rollback (the rare bad_alloc path) so the
    /// two cannot drift in their wakeup behaviour.
    static void decrement_inflight_(
        const std::shared_ptr<detail::ScopeState>& st) noexcept {
        if (!st) return;
        if (st->inflight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Last one out: fire drain waiters and notify cv.
            std::vector<std::function<void()>> waiters;
            {
                std::lock_guard lk(st->mu);
                waiters.swap(st->drain_waiters);
            }
            st->cv.notify_all();
            for (auto& w : waiters) {
                try { w(); } catch (...) {}
            }
        } else {
            // Not the last; still wake any timed waiters that may
            // want to observe progress (cheap).
            st->cv.notify_all();
        }
    }

    /// Tiny wrapper coroutine whose only job is to own `factory` for
    /// the duration of the user task. See the comment on `launch()` for
    /// the full rationale (lambda-coroutine captures are NOT copied
    /// into the coroutine frame; we make them coroutine *parameters*
    /// here so they are).
    template<typename Fn>
    static Task<void> launch_owner_coro_(Fn factory, CancellationToken tok) {
        co_await factory(std::move(tok));
    }

    /// Wrap a `Task<void>` so the scope can track its lifetime. The
    /// wrapper:
    ///   1. Increments `inflight` *before* the task starts running.
    ///   2. Awaits the task in a try/catch — `OperationCancelled` is
    ///      silently swallowed (expected under cancellation), any other
    ///      exception is forwarded to the async error sink.
    ///   3. Decrements `inflight`, notifies the cv, and fires any
    ///      `drain_waiters` registered by `join()`.
    void spawn_tracked_(Task<void> task) {
        // Snapshot the shared accounting block — wrapper holds it by
        // shared_ptr so even if the scope object is destroyed first,
        // the bookkeeping completes safely.
        auto st = state_;
        st->inflight.fetch_add(1, std::memory_order_acq_rel);
        // Detached driver coroutine — captures `st` by value. If
        // building or starting the driver throws (e.g. bad_alloc when
        // the coroutine frame can't be allocated), we must roll back
        // the inflight increment so the scope can still be drained.
        try {
            spawn_driver_(std::move(task), std::move(st)).start_detached_();
        } catch (...) {
            decrement_inflight_(state_);
            throw;
        }
    }

    static Task<void> spawn_driver_(Task<void> body,
                                    std::shared_ptr<detail::ScopeState> st) {
        // Local guard: even if the body throws synchronously before its
        // first co_await (which a properly written Task should not, but
        // we don't want to rely on it), the inflight counter still
        // decrements via the destructor.
        struct InflightGuard {
            std::shared_ptr<detail::ScopeState> st;
            ~InflightGuard() { CoroutineScope::decrement_inflight_(st); }
        };
        InflightGuard guard{std::move(st)};
        try {
            co_await std::move(body);
        } catch (const OperationCancelled&) {
            // Expected on scope.cancel(); do nothing.
        } catch (const std::exception& e) {
            report_async_error(
                std::string("CoroutineScope: unhandled exception in launched task: ") +
                e.what());
        } catch (...) {
            report_async_error(
                "CoroutineScope: unhandled non-std exception in launched task");
        }
        co_return;
    }

    // ── Parent linkage ────────────────────────────────────────────────

    /// Wire `parent` so that when the parent's cancellation fires, this
    /// scope's source is cancelled too. We capture a `weak_ptr` to the
    /// scope's state so the callback is a no-op if the child scope was
    /// destroyed first.
    void register_parent_link_(CancellationToken parent) {
        // Hold a weak handle to our own bookkeeping, and a copy of the
        // cancellation source's state via a small helper. We can't
        // weak-ref the `CancellationSource` itself, so we instead
        // schedule a deferred `cancel()` on a snapshot of the source's
        // shared state by going through our own token's `on_cancel`.
        //
        // Implementation note: we expose this via a tiny static helper
        // that captures a shared_ptr to a "parent linker" record which
        // outlives the parent token but holds a weak_ptr to our state.
        struct Linker {
            std::weak_ptr<detail::ScopeState> w_state;
            std::weak_ptr<CancellationSourceProxy> w_src;
        };
        if (!src_proxy_) {
            src_proxy_ = std::make_shared<CancellationSourceProxy>(&src_);
        }
        auto linker = std::make_shared<Linker>(
            Linker{std::weak_ptr<detail::ScopeState>(state_),
                   std::weak_ptr<CancellationSourceProxy>(src_proxy_)});
        parent.on_cancel([linker]() mutable {
            if (auto p = linker->w_src.lock()) p->cancel_safely();
        });
    }

    /// Tiny indirection so `register_parent_link_` can hold a weak_ptr
    /// to "the live source". Direct `weak_ptr<CancellationSource>` is
    /// not possible (the source is a value member). The proxy is owned
    /// by `src_proxy_` and zeroed in our destructor *before* the source
    /// itself goes away.
    struct CancellationSourceProxy {
        explicit CancellationSourceProxy(CancellationSource* s) : src_(s) {}
        void cancel_safely() noexcept {
            std::lock_guard lk(m_);
            if (src_) {
                try { src_->cancel(); } catch (...) {}
            }
        }
        void detach() noexcept {
            std::lock_guard lk(m_);
            src_ = nullptr;
        }
        std::mutex m_;
        CancellationSource* src_;
    };

    // ── Members ───────────────────────────────────────────────────────

    CancellationSource src_;
    std::shared_ptr<detail::ScopeState> state_;
    std::shared_ptr<CancellationSourceProxy> src_proxy_;

    // Detach the proxy *before* `src_` is destroyed, so any late parent
    // cancellation finds a safely-zeroed pointer.
    struct ProxyDetacher {
        std::shared_ptr<CancellationSourceProxy>* p;
        ~ProxyDetacher() {
            if (p && *p) (*p)->detach();
        }
    };
    // Order of declaration matters: `proxy_detacher_` is destroyed
    // BEFORE `src_proxy_` and `src_` (members destroyed in reverse
    // declaration order). That lets us null out the proxy's raw pointer
    // while the source is still alive — no UAF window.
    ProxyDetacher proxy_detacher_{&src_proxy_};
};

/// Awaitable that suspends the current coroutine until cancellation
/// fires on `tok`. It does NOT throw on resume — that responsibility
/// belongs to the coroutine body, via an explicit `throw_if_cancelled()`
/// probe:
///
///   while (true) {
///       tok.throw_if_cancelled();  // probe — throws if cancelled
///       co_await tok;              // park until cancel arrives
///   }
///
/// In most code you just want to PROBE periodically — use the explicit
/// `tok.throw_if_cancelled()` instead.
///
/// Race-awareness contract (must hold on every supported toolchain,
/// including MSVC and MinGW UCRT64):
///
///   1. The cancellation callback MUST NOT call `h.resume()` directly
///      from inside the cancellation broadcast loop. Doing so means the
///      resumed coroutine runs deeply nested below `cancel()`, with the
///      following frames on the stack at the moment any in-coroutine
///      `try/catch` handler matches an exception:
///
///          cancel()
///            └─ for c in cbs              (callbacks loop, holds vector)
///                └─ std::function::operator()
///                    └─ awaiter cb lambda
///                        └─ h.resume()    (the parked coroutine runs here)
///                            └─ ... user code throws ...
///
///      MSVC release builds were observed to silently bypass the
///      coroutine's own `try/catch` blocks under exactly this stack
///      shape; MinGW UCRT64 went further and SIGSEGV'd inside the
///      exception unwind. Both are symptoms of the SEH / DWARF
///      personality routine getting confused about which try/catch
///      ranges are live when the throwing PC sits inside a coroutine
///      frame whose execution has been re-entered through an
///      `std::function::operator()` indirection.
///
///   2. To eliminate the nesting, cancellation callbacks instead
///      *defer* the resume: they push the coroutine handle onto a
///      thread-local pending list, and the very topmost cancel() that
///      started the broadcast drains that list once all callbacks have
///      returned. Resumption therefore happens on the cancel() function
///      frame itself — a plain C++ function frame, free of
///      `std::function` indirection — and any exception unwind out of
///      the resumed coroutine sees a vanilla call stack that the
///      personality routine handles correctly on every toolchain.
///
///   3. `await_suspend` MUST NOT cause `h.resume()` to run on the
///      current stack — neither directly, nor indirectly through a
///      cancellation callback that fires synchronously while we are
///      still inside `await_suspend`. Touching the coroutine frame
///      after such an in-stack resume is undefined behaviour. Returning
///      `false` lets the compiler resume the caller in place; we use a
///      tiny shared state machine to coordinate that race with the
///      callback path.
///
/// State machine:
///
///   * `preparing`                   — `await_suspend` is registering
///                                     the callback and has not
///                                     committed to suspension yet.
///   * `suspended`                   — the coroutine is parked; a
///                                     future cancellation callback
///                                     must defer-resume it.
///   * `cancellation_before_suspend` — cancellation was observed while
///                                     still preparing; `await_suspend`
///                                     must return false and let the
///                                     compiler continue the coroutine.
///   * `resumed`                     — the callback has scheduled a
///                                     deferred resume of the parked
///                                     coroutine.
inline auto operator co_await(CancellationToken tok) {
    struct Latch {
        enum State : int {
            preparing = 0,
            suspended = 1,
            cancellation_before_suspend = 2,
            resumed = 3,
        };

        std::atomic<int> state{preparing};
    };
    struct Awaiter {
        CancellationToken tok;
        std::shared_ptr<Latch> latch;

        bool await_ready() const noexcept { return tok.is_cancelled(); }

        bool await_suspend(std::coroutine_handle<> h) {
            auto l = latch;
            // Register the cancellation callback. It may fire
            // synchronously right here if `tok` is already cancelled,
            // or asynchronously later from another thread. Synchronous
            // firing while we are still preparing records cancellation
            // without resuming on this stack.
            tok.on_cancel([h, l]() mutable {
                int observed = l->state.load(std::memory_order_acquire);
                for (;;) {
                    if (observed == Latch::preparing) {
                        if (l->state.compare_exchange_weak(
                                observed,
                                Latch::cancellation_before_suspend,
                                std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
                            return;
                        }
                        continue;
                    }
                    if (observed == Latch::suspended) {
                        if (l->state.compare_exchange_weak(
                                observed,
                                Latch::resumed,
                                std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
                            // Defer the resume: see contract (1)/(2)
                            // above. The topmost cancel() on this
                            // thread will drain the pending list once
                            // all callbacks return.
                            detail::schedule_deferred_resume(h);
                            return;
                        }
                        continue;
                    }
                    return;
                }
            });

            // Commit to suspension only if no cancellation callback won
            // while we were registering it. If the callback already moved
            // the state to `cancellation_before_suspend`, returning false
            // lets the compiler resume safely on the caller's stack.
            int expected = Latch::preparing;
            return l->state.compare_exchange_strong(
                expected,
                Latch::suspended,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
        }

        // Intentionally non-throwing. The body must follow up with an
        // explicit `tok.throw_if_cancelled()` probe (see the class
        // comment for why we don't throw here).
        void await_resume() noexcept {}
    };
    return Awaiter{std::move(tok), std::make_shared<Latch>()};
}

}  // namespace aria::async
