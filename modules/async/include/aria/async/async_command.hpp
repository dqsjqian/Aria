#pragma once

// AsyncCommand<R, Args...>
//
// A command whose action is asynchronous (returns Task<R>). Automatically
// tracks:
//   - is_executing : Property<bool>      (true while ANY invocation is in flight)
//   - last_error          : Property<std::optional<aria::Error>>  (nullopt when fine)
//   - last_error_message  : Property<std::string>                  ("" when fine; convenience projection)
//   - last_result         : Property<std::optional<R>>             (only when R != void)
//
// All Property mutations happen on the UI executor that you pass at
// construction. The view binds to those Properties and never sees a Task.
//
// ── UI executor contract ───────────────────────────────────────────────
// **The `ui` executor you pass to the constructor MUST be the reactive
// graph thread** — i.e. the same executor that owns the Properties this
// command's coroutine writes back to (`is_executing`, `last_error`,
// `last_result`). The reactive graph is strictly single-threaded; writing
// a Property from an unrelated thread will trip the graph-thread assert
// (and, in Release, may glitch observers).
//
// In the typical app layout the UI executor and the graph thread are the
// same thing by design: the main thread pumps both. If you are running
// headless tests or non-GUI contexts, make sure the `ui` you pass is
// the executor you pinned your Properties to — not just "some executor
// that eventually delivers messages to main".
//
// ── Action signature ───────────────────────────────────────────────────
// The action callable you pass to the constructor may have one of two
// shapes:
//
//   Task<R> action(Args...)                         // "plain" shape
//   Task<R> action(CancellationToken, Args...)      // "cancellable" shape
//
// Use the second form when your action contains cooperative
// cancellation points. The token is scoped to a SINGLE invocation of
// `execute` — cancelling the command (dtor, latest_only reset, etc.)
// flips it; a brand-new invocation gets a fresh token.
//
// ── Concurrency policy ─────────────────────────────────────────────────
// Optional third constructor argument (`AsyncCommandPolicy`):
//
//   Parallel        (default) — every execute() starts its own coroutine;
//                   is_executing stays true until the LAST finishes.
//   LatestOnly      — a new execute() cancels ANY in-flight invocations
//                   first. Ideal for search-as-you-type, filtered lists.
//   DropIfRunning   — silently ignore execute() while any invocation is
//                   running. Ideal for "Save" buttons that must not
//                   double-fire.
//
// ── Lifetime safety ────────────────────────────────────────────────────
// Every piece of state that the coroutine body touches lives in a shared
// control block held by `std::shared_ptr<SharedState>`.  The coroutine
// captures ONLY this shared_ptr — never `this`.  Consequences:
//   1. If AsyncCommand is destroyed while a coroutine is still running, the
//      `state_` block is kept alive by the coroutine until it completes.
//   2. AsyncCommand's destructor cancels `state_->cancel`, which causes the
//      next probe inside the coroutine to throw OperationCancelled, and the
//      coroutine unwinds cleanly.
//
// To keep the *user-facing* API identical to the old version
// (`cmd.is_executing.bind(...)`), AsyncCommand exposes `Property<T>&`
// members that forward into `state_`.  Those references are valid as long
// as the AsyncCommand itself is alive (which is the natural lifetime of
// user bindings).
//
// ── Internal structure ─────────────────────────────────────────────────
// The concurrency machinery shared by both the `R != void` primary
// template and the `R == void` specialisation lives in
// `detail::AsyncCommandCore<R, Args...>`:
//
//   * `make_action_<Fn>()` — normalises "plain" vs "cancellable" shapes
//   * `accept_new_invocation_()` / `cancel_all_in_flight_()`
//   * `detail::Invocation<R>` — RAII: constructing it registers a
//     per-invocation CancellationSource, flips is_executing / inflight
//     on first entry, exposes the cmd / invocation tokens; destructor
//     reverses the ledger on last exit.
//
// The derived shells (primary + void specialisation) hold a
// `AsyncCommandCore` by **composition** (not inheritance). This keeps
// the derived code free of `this->` / `typename Mixin::` dependent-name
// noise — the members read cleanly as `core_.accept_new_invocation_()`.
//
// The two `run_to_result_` bodies are kept separate on purpose: the
// `co_return co_await` vs `co_await` syntactic split is intrinsic to
// C++20 coroutines for `T` vs `void`, and forcing them into a single
// `if constexpr` branch hurts readability more than the handful of
// duplicated lines ever could. Both are noexcept-by-design — every
// failure path folds into an `AsyncCommandResult<R>` outcome instead
// of propagating an exception out of the coroutine.

#include "aria/async/async_command_result.hpp"
#include "aria/async/async_error_sink.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/error.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/executor_traits.hpp"
#include "aria/async/safe_run.hpp"
#include "aria/async/task.hpp"
#include "aria/async/timeout.hpp"  // for TimeoutError detection in classify_async_exception

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace aria::async {

/// Concurrency strategy when `execute()` is called while another
/// invocation is already running. See the file header for rationale.
enum class AsyncCommandPolicy {
    Parallel,        ///< default — all invocations run concurrently
    LatestOnly,      ///< cancel any in-flight invocations before starting
    DropIfRunning,   ///< silently ignore new invocations while busy
};

namespace detail {

/// Runtime executor-safety check used by the `IExecutor&` constructor
/// overload. Throws `std::invalid_argument` with a clear diagnostic if
/// `ui` cannot serve as a graph executor, `worker` cannot serve as a
/// worker, or the pair is the known-broken "Inline graph + non-Inline
/// worker" combination.
inline void check_executor_safety_runtime(IExecutor& ui, IExecutor& worker) {
    if (!ui.is_safe_graph_executor()) {
        throw std::invalid_argument(
            "AsyncCommand: ui executor is not safe to use as the "
            "graph-thread executor. Override "
            "IExecutor::is_safe_graph_executor() in your subclass, or "
            "use MainThreadExecutor.");
    }
    if (!worker.is_safe_worker_executor()) {
        throw std::invalid_argument(
            "AsyncCommand: worker executor is not safe to host worker "
            "tasks. Override IExecutor::is_safe_worker_executor() in "
            "your subclass, or use ThreadPoolExecutor / "
            "MainThreadExecutor.");
    }
    auto* ui_inline     = dynamic_cast<InlineExecutor*>(&ui);
    auto* worker_inline = dynamic_cast<InlineExecutor*>(&worker);
    if (ui_inline && !worker_inline) {
        throw std::invalid_argument(
            "AsyncCommand: cannot use InlineExecutor as the "
            "graph-thread executor when worker runs on a different "
            "thread. The final co_await schedule_on(ui) would "
            "inline-resume on the worker thread and write reactive "
            "Properties from there, tripping the graph thread-affinity "
            "invariant. Use MainThreadExecutor for the ui parameter.");
    }
}

/// Observable state shared between AsyncCommand and its in-flight coroutines.
/// A shared_ptr to this is the ONLY thing the coroutine body captures.
template<typename R, typename... Args>
struct AsyncCommandState {
    using ArgsTuple = std::tuple<Args...>;

    IExecutor* ui;
    IExecutor* worker;
    CancellationSource cancel;            // dtor cancels all running coroutines
    std::atomic<int> inflight{0};

    Property<bool>                                  is_executing{false};
    Property<std::optional<::aria::Error>>          last_error{std::nullopt};
    Property<std::string>                           last_error_message{""};
    Property<std::optional<R>>                      last_result{std::optional<R>{}};

    std::mutex m_sources;
    std::vector<std::shared_ptr<CancellationSource>> invocation_sources;

    AsyncCommandState(IExecutor& u, IExecutor& w) : ui(&u), worker(&w) {}
};

template<>
struct AsyncCommandState<void> {
    IExecutor* ui;
    IExecutor* worker;
    CancellationSource cancel;
    std::atomic<int> inflight{0};

    Property<bool>                          is_executing{false};
    Property<std::optional<::aria::Error>>  last_error{std::nullopt};
    Property<std::string>                   last_error_message{""};

    std::mutex m_sources;
    std::vector<std::shared_ptr<CancellationSource>> invocation_sources;

    AsyncCommandState(IExecutor& u, IExecutor& w) : ui(&u), worker(&w) {}
};

/// Outcome category produced by `classify_async_exception`. Mirrors
/// the four AsyncCommandStatus categories minus the success path; the
/// caller selects between `Completed` (no exception) and one of these
/// before assembling the final AsyncCommandResult.
enum class AsyncFailureKind : std::uint8_t {
    Cancellation,   ///< OperationCancelled was thrown
    Failure,        ///< any other exception (timeout / domain / std)
};

struct AsyncFailureClassification {
    AsyncFailureKind kind;
    ::aria::Error    error;
};

/// Inspect a captured exception, side-effect the observable Properties,
/// and return a structured classification.
///
/// Contract changes from the legacy `process_async_exception`:
///   * Never rethrows. The caller (`run_to_result_`) is responsible
///     for shaping the final AsyncCommandResult. This is what makes
///     `co_execute` exception-safe end-to-end without wrapping the
///     coroutine body in another try/catch.
///   * Cancellation does not write to `last_error` (cancellation is
///     not an observable user-facing error), but it IS classified and
///     surfaces in the AsyncCommandResult so awaiters can branch on
///     `r.cancelled()`.
///   * Trace events are emitted on the same three branches as before
///     (cancelled / timeout / failure).
///
/// Precondition: `ex` must be non-null. The caller already checked.
inline AsyncFailureClassification classify_async_exception(
    std::exception_ptr ex,
    Property<std::optional<::aria::Error>>& last_error,
    Property<std::string>& last_error_message,
    std::string source_tag = "AsyncCommand")
{
    try { std::rethrow_exception(ex); }
    catch (const OperationCancelled&) {
        auto err = ::aria::Error::cancellation(source_tag);
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                ::aria::trace::Async{source_tag, "cancelled", 0},
                err);
        }
        // Intentionally NOT touching last_error: cancellation is not
        // an observable failure. Awaiters still see r.cancelled() via
        // the returned classification.
        return {AsyncFailureKind::Cancellation, std::move(err)};
    }
    catch (const TimeoutError& e) {
        auto err = ::aria::Error::timeout(source_tag);
        err.message = e.what();
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                ::aria::trace::Async{source_tag, "timeout", 0},
                err);
        }
        last_error          = err;
        last_error_message  = err.message;
        return {AsyncFailureKind::Failure, std::move(err)};
    }
    catch (...) {
        auto err = ::aria::Error::from_exception(ex, source_tag);
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                ::aria::trace::Async{source_tag, "failure", 0},
                err);
        }
        last_error_message  = err.message;
        last_error          = err;
        return {AsyncFailureKind::Failure, std::move(err)};
    }
}

template<typename F, typename... Args>
concept CancellableAction =
    std::invocable<F, CancellationToken, Args...>;

template<typename F, typename... Args>
concept PlainAction =
    std::invocable<F, Args...>;

/// RAII handle representing one in-flight invocation.
///
/// Constructing an `Invocation` for a given state block:
///   1. allocates a per-invocation CancellationSource,
///   2. registers it with the command-wide source list (so LatestOnly
///      can cancel this run without touching others),
///   3. bumps inflight; on the 0→1 edge it flips is_executing = true
///      and clears last_error.
///
/// Destruction reverses the ledger: deregisters the source and, on
/// the 1→0 edge, flips is_executing = false.
///
/// This collapses the older `begin_invocation_()` + `InflightGuard`
/// pair into a single object with a clear lifetime.
template<typename R>
class Invocation {
public:
    using State = AsyncCommandState<R>;

    explicit Invocation(std::shared_ptr<State> s)
        : state_(std::move(s)),
          src_(std::make_shared<CancellationSource>()),
          cmd_tok_(state_->cancel.token()),
          inv_tok_(src_->token())
    {
        {
            std::lock_guard lk(state_->m_sources);
            state_->invocation_sources.push_back(src_);
        }
        const bool first = (state_->inflight.fetch_add(1, std::memory_order_acq_rel) == 0);
        if (first) {
            state_->is_executing       = true;
            state_->last_error         = std::nullopt;
            state_->last_error_message = "";
        }
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                ::aria::trace::Async{
                    "AsyncCommand",
                    "execute_start",
                    static_cast<std::uint64_t>(state_->inflight.load(std::memory_order_relaxed)),
                });
        }
    }

    ~Invocation() {
        {
            std::lock_guard lk(state_->m_sources);
            auto& v = state_->invocation_sources;
            v.erase(std::remove(v.begin(), v.end(), src_), v.end());
        }
        const bool last = (state_->inflight.fetch_sub(1, std::memory_order_acq_rel) == 1);
        if (last) {
            state_->is_executing = false;
        }
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Async,
                ::aria::trace::Async{
                    "AsyncCommand",
                    "execute_finish",
                    static_cast<std::uint64_t>(state_->inflight.load(std::memory_order_relaxed)),
                });
        }
    }

    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;

    const CancellationToken& cmd_tok() const noexcept { return cmd_tok_; }
    const CancellationToken& inv_tok() const noexcept { return inv_tok_; }

    /// Throws `OperationCancelled` if either token is flipped.
    void throw_if_cancelled() const {
        cmd_tok_.throw_if_cancelled();
        inv_tok_.throw_if_cancelled();
    }

private:
    std::shared_ptr<State> state_;
    std::shared_ptr<CancellationSource> src_;
    CancellationToken cmd_tok_;
    CancellationToken inv_tok_;
};

/// Core holds the concurrency machinery shared by both `AsyncCommand`
/// specialisations. Held by **composition** inside each derived shell —
/// no inheritance, no dependent-name noise on the caller side.
template<typename R, typename... Args>
class AsyncCommandCore {
public:
    using State     = AsyncCommandState<R>;
    using Action    = std::function<Task<R>(CancellationToken, Args...)>;
    using ArgsTuple = std::shared_ptr<std::tuple<Args...>>;

    AsyncCommandCore(std::shared_ptr<State> s, Action a, AsyncCommandPolicy p)
        : state(std::move(s)), action(std::move(a)), policy(p) {}

    std::shared_ptr<State> state;
    Action action;
    AsyncCommandPolicy policy;

    /// Adapt user's action (plain or cancellable) to the unified
    /// internal `(CancellationToken, Args...)` signature. The void vs
    /// non-void branches are both necessary: `co_return co_await` is
    /// ill-formed for a void-returning coroutine.
    template<typename Fn>
    static Action make_action(Fn f) {
        if constexpr (CancellableAction<Fn, Args...>) {
            return Action(std::move(f));
        } else if constexpr (std::is_void_v<R>) {
            return [f = std::move(f)](CancellationToken,
                                       Args... a) mutable -> Task<void> {
                co_await f(std::move(a)...);
            };
        } else {
            return [f = std::move(f)](CancellationToken,
                                       Args... a) mutable -> Task<R> {
                co_return co_await f(std::move(a)...);
            };
        }
    }

    /// Decide whether a new invocation may start. LatestOnly cancels
    /// whatever is running first; DropIfRunning returns false.
    bool accept_new_invocation() {
        switch (policy) {
        case AsyncCommandPolicy::Parallel:
            return true;
        case AsyncCommandPolicy::LatestOnly:
            cancel_all_in_flight();
            return true;
        case AsyncCommandPolicy::DropIfRunning:
            return state->inflight.load(std::memory_order_acquire) == 0;
        }
        return true;
    }

    void cancel_all_in_flight() {
        std::vector<std::shared_ptr<CancellationSource>> victims;
        {
            std::lock_guard lk(state->m_sources);
            victims = state->invocation_sources;
        }
        for (auto& s : victims) s->cancel();
    }
};

}  // namespace detail

// ── primary template (R != void) ──────────────────────────────────────
template<typename R, typename... Args>
class AsyncCommand {
    using Core       = detail::AsyncCommandCore<R, Args...>;
    using State      = typename Core::State;
    using ArgsTuple  = typename Core::ArgsTuple;
    using Invocation = detail::Invocation<R>;

    Core core_;

public:
    using Action = typename Core::Action;

    /// Construct with a "plain" or "cancellable" action. The shape is
    /// detected at compile time; if you pass a `(Args...)` callable, we
    /// synthesise a wrapper that discards the token. If your callable
    /// is ambiguous (happens with generic lambdas), cast it to the
    /// desired std::function first.
    ///
    /// Executor safety is enforced as eagerly as possible:
    ///   * If the static types of `ui` and `worker` are concrete
    ///     subclasses of `IExecutor`, this template overload picks them
    ///     up and rejects unsafe combinations at **compile time** via
    ///     the static_asserts below.
    ///   * If `ui` / `worker` are `IExecutor&`, the dedicated overload
    ///     below performs the equivalent check at construction time.
    template<typename Ui, typename Worker, typename Fn>
        requires (detail::CancellableAction<Fn, Args...>
                 || detail::PlainAction<Fn, Args...>)
                 && std::is_base_of_v<IExecutor, Ui>
                 && std::is_base_of_v<IExecutor, Worker>
                 && (!std::is_same_v<Ui, IExecutor>
                     || !std::is_same_v<Worker, IExecutor>)
    AsyncCommand(Ui& ui, Worker& worker, Fn action,
                 AsyncCommandPolicy policy = AsyncCommandPolicy::Parallel)
        : core_(std::make_shared<State>(ui, worker),
                Core::template make_action<Fn>(std::move(action)),
                policy),
          is_executing       (core_.state->is_executing),
          last_error         (core_.state->last_error),
          last_error_message (core_.state->last_error_message),
          last_result        (core_.state->last_result)
    {
        if constexpr (!std::is_same_v<Ui, IExecutor>) {
            static_assert(is_safe_graph_executor_v<Ui>,
                "AsyncCommand: `ui` must be a graph-thread executor. "
                "Specialise `aria::async::is_safe_graph_executor<YourExec>` "
                "or use `MainThreadExecutor`.");
        }
        if constexpr (!std::is_same_v<Worker, IExecutor>) {
            static_assert(is_safe_worker_executor_v<Worker>,
                "AsyncCommand: `worker` must be a worker-capable executor. "
                "Specialise `aria::async::is_safe_worker_executor<YourExec>` "
                "or use `ThreadPoolExecutor` / `MainThreadExecutor`.");
        }
        if constexpr (!std::is_same_v<Ui, IExecutor>
                      && !std::is_same_v<Worker, IExecutor>) {
            static_assert(!(std::is_same_v<Ui, InlineExecutor>
                            && !std::is_same_v<Worker, InlineExecutor>),
                "AsyncCommand: cannot use `InlineExecutor` as the graph-thread "
                "executor when `worker` runs on a different thread. Use "
                "`MainThreadExecutor` for the `ui` parameter.");
        }
        detail::check_executor_safety_runtime(ui, worker);
    }

    /// Type-erased overload — the choice for ViewModels that receive
    /// `IExecutor&` from a DI container.
    template<typename Fn>
        requires detail::CancellableAction<Fn, Args...>
                 || detail::PlainAction<Fn, Args...>
    AsyncCommand(IExecutor& ui, IExecutor& worker, Fn action,
                 AsyncCommandPolicy policy = AsyncCommandPolicy::Parallel)
        : core_(std::make_shared<State>(ui, worker),
                Core::template make_action<Fn>(std::move(action)),
                policy),
          is_executing       (core_.state->is_executing),
          last_error         (core_.state->last_error),
          last_error_message (core_.state->last_error_message),
          last_result        (core_.state->last_result)
    {
        detail::check_executor_safety_runtime(ui, worker);
    }

    ~AsyncCommand() {
        // Signal every in-flight coroutine to bail on its next probe.
        // state lives on as long as some coroutine still references it,
        // so Property writes inside those coroutines remain safe.
        core_.state->cancel.cancel();
    }

    AsyncCommand(const AsyncCommand&) = delete;
    AsyncCommand& operator=(const AsyncCommand&) = delete;

    /// Fire-and-forget. Concurrency shape controlled by `policy_`.
    /// Errors are reported via the installed `error_sink_` and the
    /// observable `last_error` Property; this function itself is
    /// noexcept-by-design (the wrapper coroutine cannot propagate).
    void execute(Args... args) {
        if (!core_.accept_new_invocation()) return;
        auto tup = std::make_shared<std::tuple<Args...>>(std::move(args)...);
        fire_and_forget_(tup).start_detached_();
    }

    /// Awaitable version: caller co_awaits a structured result.
    ///
    /// **Never throws.** All four invocation outcomes — Completed,
    /// Dropped (policy=DropIfRunning while busy), Cancelled
    /// (OperationCancelled at any cancellation point), Failed (action
    /// threw any other exception) — are folded into the returned
    /// AsyncCommandResult. Callers branch on `.completed()` /
    /// `.dropped()` / `.cancelled()` / `.failed()` instead of writing
    /// `try { co_await ... } catch (...)` boilerplate around every
    /// invocation site.
    Task<AsyncCommandResult<R>> co_execute(Args... args) {
        if (!core_.accept_new_invocation()) {
            co_return AsyncCommandResult<R>::dropped_();
        }
        auto tup = std::make_shared<std::tuple<Args...>>(std::move(args)...);
        co_return co_await run_to_result_(tup);
    }

    [[nodiscard]] AsyncCommandPolicy policy() const noexcept { return core_.policy; }

    // Public observable handles.
    Property<bool>&                          is_executing;
    Property<std::optional<::aria::Error>>&  last_error;
    Property<std::string>&                   last_error_message;
    Property<std::optional<R>>&              last_result;

private:
    /// Fire-and-forget wrapper. The Result-based core path means we
    /// never observe an exception here — but we still route Failed
    /// outcomes through the installed error sink so production builds
    /// are not silent (matches legacy `execute()` observable behaviour).
    Task<void> fire_and_forget_(ArgsTuple args) {
        auto r = co_await run_to_result_(std::move(args));
        if (r.failed() && r.error) {
            report_async_error(std::string("AsyncCommand: ") + r.error->message);
        }
        // Cancelled / Completed / (Dropped — never reaches here, the
        // policy gate above filters it out before run_to_result_) are
        // intentionally silent on the sink path; observers learn about
        // them via is_executing / last_result Properties.
    }

    /// Single source of truth for invocation execution. Never throws.
    /// Both `execute()` (via fire_and_forget_) and `co_execute()`
    /// share this body; the only difference between the two public
    /// entry points is whether the caller co_awaits the result.
    Task<AsyncCommandResult<R>> run_to_result_(ArgsTuple args) {
        auto state  = core_.state;
        auto action = core_.action;
        Invocation inv{state};

        co_await schedule_on(*state->ui);
        // Pre-flight cancellation check — if the command was cancelled
        // between accept_new_invocation() and our first hop onto the
        // ui executor, surface that as Cancelled without ever invoking
        // the user's action.
        if (inv.cmd_tok().is_cancelled() || inv.inv_tok().is_cancelled()) {
            co_return AsyncCommandResult<R>::cancelled_(
                ::aria::Error::cancellation("AsyncCommand"));
        }

        std::optional<R> value;
        std::exception_ptr ex;
        try {
            co_await schedule_on(*state->worker);
            inv.throw_if_cancelled();
            value.emplace(co_await std::apply(
                [&](auto&&... a) -> Task<R> {
                    return action(inv.inv_tok(), std::forward<decltype(a)>(a)...);
                }, *args));
        } catch (...) {
            ex = std::current_exception();
        }

        co_await schedule_on(*state->ui);
        if (ex) {
            auto cls = detail::classify_async_exception(
                ex, state->last_error, state->last_error_message);
            if (cls.kind == detail::AsyncFailureKind::Cancellation) {
                co_return AsyncCommandResult<R>::cancelled_(std::move(cls.error));
            }
            co_return AsyncCommandResult<R>::failed_(std::move(cls.error));
        }
        state->last_result = value;
        co_return AsyncCommandResult<R>::completed_with(std::move(*value));
    }
};

// ── partial specialisation (R == void) ────────────────────────────
template<typename... Args>
class AsyncCommand<void, Args...> {
    using Core       = detail::AsyncCommandCore<void, Args...>;
    using State      = typename Core::State;
    using ArgsTuple  = typename Core::ArgsTuple;
    using Invocation = detail::Invocation<void>;

    Core core_;

public:
    using Action = typename Core::Action;

    template<typename Ui, typename Worker, typename Fn>
        requires (detail::CancellableAction<Fn, Args...>
                 || detail::PlainAction<Fn, Args...>)
                 && std::is_base_of_v<IExecutor, Ui>
                 && std::is_base_of_v<IExecutor, Worker>
                 && (!std::is_same_v<Ui, IExecutor>
                     || !std::is_same_v<Worker, IExecutor>)
    AsyncCommand(Ui& ui, Worker& worker, Fn action,
                 AsyncCommandPolicy policy = AsyncCommandPolicy::Parallel)
        : core_(std::make_shared<State>(ui, worker),
                Core::template make_action<Fn>(std::move(action)),
                policy),
          is_executing       (core_.state->is_executing),
          last_error         (core_.state->last_error),
          last_error_message (core_.state->last_error_message)
    {
        if constexpr (!std::is_same_v<Ui, IExecutor>) {
            static_assert(is_safe_graph_executor_v<Ui>,
                "AsyncCommand<void>: `ui` must be a graph-thread executor. "
                "Use `MainThreadExecutor` for any multi-threaded scenario.");
        }
        if constexpr (!std::is_same_v<Worker, IExecutor>) {
            static_assert(is_safe_worker_executor_v<Worker>,
                "AsyncCommand<void>: `worker` must be a worker-capable executor.");
        }
        if constexpr (!std::is_same_v<Ui, IExecutor>
                      && !std::is_same_v<Worker, IExecutor>) {
            static_assert(!(std::is_same_v<Ui, InlineExecutor>
                            && !std::is_same_v<Worker, InlineExecutor>),
                "AsyncCommand<void>: cannot use `InlineExecutor` as the "
                "graph-thread executor when `worker` runs on a different thread. "
                "Use `MainThreadExecutor` for `ui`.");
        }
        detail::check_executor_safety_runtime(ui, worker);
    }

    /// Type-erased overload — same role as on the primary template:
    /// the choice for ViewModels that receive `IExecutor&` from a DI
    /// container. Mirrors the concrete-typed overload above; safety
    /// invariants are checked at construction time via
    /// `check_executor_safety_runtime`.
    template<typename Fn>
        requires detail::CancellableAction<Fn, Args...>
                 || detail::PlainAction<Fn, Args...>
    AsyncCommand(IExecutor& ui, IExecutor& worker, Fn action,
                 AsyncCommandPolicy policy = AsyncCommandPolicy::Parallel)
        : core_(std::make_shared<State>(ui, worker),
                Core::template make_action<Fn>(std::move(action)),
                policy),
          is_executing       (core_.state->is_executing),
          last_error         (core_.state->last_error),
          last_error_message (core_.state->last_error_message)
    {
        detail::check_executor_safety_runtime(ui, worker);
    }

    ~AsyncCommand() { core_.state->cancel.cancel(); }

    AsyncCommand(const AsyncCommand&) = delete;
    AsyncCommand& operator=(const AsyncCommand&) = delete;

    /// Fire-and-forget. Errors are reported via the installed
    /// `error_sink_` and `last_error`; this function never throws.
    void execute(Args... args) {
        if (!core_.accept_new_invocation()) return;
        auto tup = std::make_shared<std::tuple<Args...>>(std::move(args)...);
        fire_and_forget_(tup).start_detached_();
    }

    /// Awaitable version: caller co_awaits a structured result.
    /// Never throws — see the primary template's co_execute() for
    /// the four-outcome contract. The void specialisation has no
    /// `value`; only status + optional error.
    Task<AsyncCommandResult<void>> co_execute(Args... args) {
        if (!core_.accept_new_invocation()) {
            co_return AsyncCommandResult<void>::dropped_();
        }
        auto tup = std::make_shared<std::tuple<Args...>>(std::move(args)...);
        co_return co_await run_to_result_(tup);
    }

    [[nodiscard]] AsyncCommandPolicy policy() const noexcept { return core_.policy; }

    Property<bool>&                          is_executing;
    Property<std::optional<::aria::Error>>&  last_error;
    Property<std::string>&                   last_error_message;

private:
    Task<void> fire_and_forget_(ArgsTuple args) {
        auto r = co_await run_to_result_(std::move(args));
        if (r.failed() && r.error) {
            report_async_error(std::string("AsyncCommand: ") + r.error->message);
        }
    }

    /// Single source of truth for invocation execution. Never throws.
    Task<AsyncCommandResult<void>> run_to_result_(ArgsTuple args) {
        auto state  = core_.state;
        auto action = core_.action;
        Invocation inv{state};

        co_await schedule_on(*state->ui);
        if (inv.cmd_tok().is_cancelled() || inv.inv_tok().is_cancelled()) {
            co_return AsyncCommandResult<void>::cancelled_(
                ::aria::Error::cancellation("AsyncCommand"));
        }

        std::exception_ptr ex;
        try {
            co_await schedule_on(*state->worker);
            inv.throw_if_cancelled();
            co_await std::apply(
                [&](auto&&... a) -> Task<void> {
                    return action(inv.inv_tok(), std::forward<decltype(a)>(a)...);
                }, *args);
        } catch (...) {
            ex = std::current_exception();
        }

        co_await schedule_on(*state->ui);
        if (ex) {
            auto cls = detail::classify_async_exception(
                ex, state->last_error, state->last_error_message);
            if (cls.kind == detail::AsyncFailureKind::Cancellation) {
                co_return AsyncCommandResult<void>::cancelled_(std::move(cls.error));
            }
            co_return AsyncCommandResult<void>::failed_(std::move(cls.error));
        }
        co_return AsyncCommandResult<void>::completed_with();
    }
};

}  // namespace aria::async
