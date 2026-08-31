#pragma once

// when_all / when_any combinators for aria::async::Task<T>.
//
// Usage:
//   auto [a, b, c] = co_await when_all(fetchA(), fetchB(), fetchC());
//   auto first     = co_await when_any(slow(), fast(), medium());
//
//   // Cancellable form: each factory receives a CancellationToken;
//   // losers are notified via their token once a winner emerges.
//   std::vector<std::function<Task<int>(CancellationToken)>> fs = {
//       [](CancellationToken t) -> Task<int> { co_return co_await slow_op(t); },
//       [](CancellationToken t) -> Task<int> { co_return co_await fast_op(t); },
//   };
//   auto first = co_await when_any_cancellable(std::move(fs));
//
// Implementation strategy:
//   when_all: each input task is started immediately and drives a shared
//             atomic counter. The combinator suspends until the counter
//             reaches the target.
//   when_any: built on detail::RaceSlot — atomic winner CAS plus a mu_-
//             serialised parent_handle so a synchronously-completing task
//             cannot resume the parent before await_suspend has finished
//             storing it.

#include "aria/async/task.hpp"
#include "aria/async/cancellation.hpp"
#include "aria/async/detail/race_slot.hpp"
#include "aria/async/detail/race_trace.hpp"

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aria::async {

namespace detail {

template<typename T>
struct WhenAllSlot {
    std::optional<T> value;
    std::exception_ptr error;
};

template<>
struct WhenAllSlot<void> {
    std::exception_ptr error;
};

/// Atomic, TSan-clean handoff of the parent coroutine handle.
///
/// `when_all`'s parent handle is written by `await_suspend` (on the
/// awaiting coroutine's thread) and read by whichever driver drives the
/// `remaining_` counter to zero (possibly a different thread). Earlier
/// revisions stored it in a plain `std::shared_ptr<std::coroutine_handle<>>`
/// and leaned on the `remaining_` acquire/release for ordering — correct
/// in practice but a non-atomic object racing across threads, which TSan
/// flags and any future reorder of `await_suspend`'s two halves would turn
/// into a genuine data race. We now publish the handle through a single
/// `std::atomic<void*>`: `store(release)` on the writer side
/// happens-before `load(acquire)` on the reader side, so the ordering is
/// enforced by the type system rather than by a "do not reorder" comment.
class WhenAllParentHandle {
public:
    /// Store the parent handle (release). Called once from await_suspend.
    void store(std::coroutine_handle<> h) noexcept {
        slot_.store(h.address(), std::memory_order_release);
    }
    /// Load the parent handle (acquire). Returns a null handle until the
    /// writer has published one.
    [[nodiscard]] std::coroutine_handle<> load() const noexcept {
        void* p = slot_.load(std::memory_order_acquire);
        return p ? std::coroutine_handle<>::from_address(p)
                 : std::coroutine_handle<>{};
    }

private:
    std::atomic<void*> slot_{nullptr};
};

/// Drive a single child Task<T>, write into slot, decrement remaining,
/// resume the parent when remaining hits zero.
template<typename T>
Task<void> drive_one(Task<T> task,
                     std::shared_ptr<WhenAllSlot<T>> slot,
                     std::shared_ptr<std::atomic<std::size_t>> remaining,
                     std::shared_ptr<WhenAllParentHandle> parent) {
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
        } else {
            slot->value.emplace(co_await std::move(task));
        }
    } catch (...) {
        slot->error = std::current_exception();
    }
    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Last participant home: when_all has no losers, so completion
        // IS the arbitration outcome (D-31.1). A participant that threw
        // does not change which event fires — the failure surfaces
        // through await_resume.
        publish_race_trace(race_source::kWhenAll, race_op::kWon);
        if (auto h = parent->load()) h.resume();
    }
}

}  // namespace detail

/// Awaitable that resolves when ALL input Tasks complete.
/// Returns std::tuple<T1, T2, ...>.
///
/// Race contract:
///   await_suspend publishes `parent_->store(caller)` (release) BEFORE
///   spawning any driver coroutine. Drivers read `parent_->load()`
///   (acquire) only after the `remaining_` atomic counter hits zero — so
///   a synchronously-completing task that drives `remaining_` to zero
///   inside its own `start_detached()` call still observes the published
///   handle. The handle is held in an atomic (`WhenAllParentHandle`), so
///   this is TSan-clean and does not depend on statement ordering for
///   correctness; the store-before-spawn ordering is kept only as a
///   performance optimisation (avoids a redundant re-check).
template<typename... Ts>
class WhenAllAwaiter {
public:
    using Result = std::tuple<Ts...>;

    explicit WhenAllAwaiter(Task<Ts>... ts)
        : tasks_(std::make_tuple(std::move(ts)...)),
          slots_(std::make_tuple(std::make_shared<detail::WhenAllSlot<Ts>>()...)) {}

    bool await_ready() const noexcept { return sizeof...(Ts) == 0; }

    void await_suspend(std::coroutine_handle<> caller) {
        parent_->store(caller);
        detail::publish_race_trace(detail::race_source::kWhenAll,
                                   detail::race_op::kStart,
                                   sizeof...(Ts));
        // Spawn drivers as fully-detached coroutines.  Each driver owns its
        // own coroutine frame via Task::start_detached — no need for the
        // awaiter to keep them alive, and no leaks.
        std::apply([this](auto&... task) {
            std::apply([this, &task...](auto&... slot) {
                (detail::drive_one(
                    std::move(task), slot, remaining_, parent_
                 ).start_detached(), ...);
            }, slots_);
        }, tasks_);
    }

    Result await_resume() {
        // Guarded by the same condition as await_ready: a zero-task
        // when_all never suspends, so it never armed a race and must not
        // publish an unpaired race_end.
        if constexpr (sizeof...(Ts) > 0) {
            detail::publish_race_trace(detail::race_source::kWhenAll,
                                       detail::race_op::kEnd);
        }
        std::exception_ptr first_err;
        std::apply([&](auto&... slot) {
            (((slot->error && !first_err) ? first_err = slot->error : nullptr), ...);
        }, slots_);

        if (first_err) std::rethrow_exception(first_err);
        return build_result_(std::index_sequence_for<Ts...>{});
    }

private:
    template<std::size_t... I>
    Result build_result_(std::index_sequence<I...>) {
        return Result{std::move(*std::get<I>(slots_)->value)...};
    }

    std::tuple<Task<Ts>...> tasks_;
    std::tuple<std::shared_ptr<detail::WhenAllSlot<Ts>>...> slots_;
    std::shared_ptr<std::atomic<std::size_t>> remaining_ =
        std::make_shared<std::atomic<std::size_t>>(sizeof...(Ts));
    std::shared_ptr<detail::WhenAllParentHandle> parent_ =
        std::make_shared<detail::WhenAllParentHandle>();
};

template<typename... Ts>
auto when_all(Task<Ts>... tasks) {
    return WhenAllAwaiter<Ts...>{std::move(tasks)...};
}

// ── when_any ────────────────────────────────────────────────────────────

namespace detail {

/// Driver coroutine for the basic `when_any(std::vector<Task<T>>)` form.
/// Tasks here have no cancellation token attached, so losers simply run
/// to completion in the background — their result and exception are
/// silently discarded.
template<typename T>
Task<void> drive_any_basic_(Task<T> task,
                            std::size_t idx,
                            std::shared_ptr<RaceSlot<T>> slot)
{
    try {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(task);
            if (slot->try_claim(/*winner=*/1)) {
                slot->winner_index = idx;
                slot->result.template emplace<1>();   // void success
                slot->publish(/*winner=*/1);
                publish_race_trace(race_source::kWhenAny, race_op::kWon, idx);
                slot->notify_winner_resume();
            }
        } else {
            T v = co_await std::move(task);
            if (slot->try_claim(/*winner=*/1)) {
                slot->winner_index = idx;
                slot->result.template emplace<1>(std::move(v));
                slot->publish(/*winner=*/1);
                publish_race_trace(race_source::kWhenAny, race_op::kWon, idx);
                slot->notify_winner_resume();
            }
        }
    } catch (...) {
        if (slot->try_claim(/*winner=*/1)) {
            slot->winner_index = idx;
            slot->result.template emplace<2>(std::current_exception());
            slot->publish(/*winner=*/1);
            // Still the winner, just with a failure; the exception
            // surfaces through await_resume rather than as its own event.
            publish_race_trace(race_source::kWhenAny, race_op::kWon, idx);
            slot->notify_winner_resume();
        }
    }
}

/// Driver coroutine for `when_any_cancellable`. Each loser receives a
/// best-effort cancellation request via its CancellationSource once a
/// winner is established, allowing cooperative inner work to unwind
/// promptly instead of running to completion in the background.
template<typename T, typename Factory>
Task<void> drive_any_cancellable_(Factory factory,
                                  std::size_t idx,
                                  std::shared_ptr<RaceSlot<T>> slot,
                                  std::shared_ptr<CancellationSource> src,
                                  std::shared_ptr<std::vector<std::shared_ptr<CancellationSource>>> all_sources)
{
    auto cancel_losers = [all_sources, idx]() {
        std::uint64_t signalled = 0;
        for (std::size_t i = 0; i < all_sources->size(); ++i) {
            if (i == idx) continue;
            if (auto& s = (*all_sources)[i]; s) {
                s->cancel();
                ++signalled;
            }
        }
        // One event per race, not per loser (D-31.1): `generation`
        // carries how many losers were actually signalled.
        publish_race_trace(race_source::kWhenAnyCancellable,
                           race_op::kLoserCancel, signalled);
    };

    CancellationToken tok = src->token();

    try {
        if constexpr (std::is_void_v<T>) {
            co_await factory(tok);
            if (slot->try_claim(/*winner=*/1)) {
                slot->winner_index = idx;
                slot->result.template emplace<1>();
                slot->publish(/*winner=*/1);
                publish_race_trace(race_source::kWhenAnyCancellable,
                                   race_op::kWon, idx);
                cancel_losers();
                slot->notify_winner_resume();
            }
        } else {
            T v = co_await factory(tok);
            if (slot->try_claim(/*winner=*/1)) {
                slot->winner_index = idx;
                slot->result.template emplace<1>(std::move(v));
                slot->publish(/*winner=*/1);
                publish_race_trace(race_source::kWhenAnyCancellable,
                                   race_op::kWon, idx);
                cancel_losers();
                slot->notify_winner_resume();
            }
        }
    } catch (...) {
        if (slot->try_claim(/*winner=*/1)) {
            slot->winner_index = idx;
            slot->result.template emplace<2>(std::current_exception());
            slot->publish(/*winner=*/1);
            publish_race_trace(race_source::kWhenAnyCancellable,
                               race_op::kWon, idx);
            cancel_losers();
            slot->notify_winner_resume();
        }
    }
}

}  // namespace detail

/// Awaitable that resolves when ANY of the input Tasks completes (success or error).
/// Returns the index (and result) of whichever finished first.
///
/// Race contract:
///   * Built on detail::RaceSlot — closes the await_suspend race window
///     where a synchronously-completing task could resume the parent
///     before await_suspend has stored the parent handle.
///   * Late tasks (losers) silently drop their results; their detached
///     driver coroutines continue to run to completion in the background
///     and eventually self-destruct.
///   * For cooperative cancellation of losers, see `when_any_cancellable`
///     below — it accepts factories that take a CancellationToken.
template<typename T>
class WhenAnyAwaiter {
public:
    // For void T the `value` slot collapses to monostate so callers can
    // still use a uniform `Result` shape (T-typed code paths can check
    // `error` and `index`).
    using ValueField = std::conditional_t<std::is_void_v<T>,
                                          std::monostate,
                                          std::optional<T>>;
    struct Result {
        std::size_t        index = std::size_t(-1);
        ValueField         value{};
        std::exception_ptr error{};
    };

    explicit WhenAnyAwaiter(std::vector<Task<T>> tasks) : tasks_(std::move(tasks)) {}

    bool await_ready() const noexcept {
        if (tasks_.empty()) return true;
        return slot_->winner.load(std::memory_order_acquire) != 0;
    }

    bool await_suspend(std::coroutine_handle<> caller) noexcept {
        detail::publish_race_trace(detail::race_source::kWhenAny,
                                   detail::race_op::kStart,
                                   tasks_.size());
        // Start all driver coroutines. Each captures `slot_` so the
        // shared race state lives as long as needed. If any driver
        // synchronously resolves the slot, our await_suspend tail
        // re-checks `winner` under `mu` and skips suspension.
        for (std::size_t i = 0; i < tasks_.size(); ++i) {
            detail::drive_any_basic_<T>(std::move(tasks_[i]), i, slot_)
                .start_detached();
        }
        std::lock_guard lk(slot_->mu);
        if (slot_->winner.load(std::memory_order_acquire) != 0) {
            return false;
        }
        slot_->parent_handle = caller;
        slot_->parent_stored = true;
        return true;
    }

    Result await_resume() {
        // An empty task list short-circuits await_ready and never armed a
        // race, so it must not publish an unpaired race_end.
        if (!tasks_.empty()) {
            detail::publish_race_trace(detail::race_source::kWhenAny,
                                       detail::race_op::kEnd);
        }
        Result r;
        r.index = slot_->winner_index;
        auto& v = slot_->result;
        if (v.index() == 2) {
            r.error = std::get<2>(v);
        } else if constexpr (!std::is_void_v<T>) {
            if (v.index() == 1) r.value = std::get<1>(std::move(v));
        }
        return r;
    }

private:
    std::vector<Task<T>> tasks_;
    std::shared_ptr<detail::RaceSlot<T>> slot_ =
        std::make_shared<detail::RaceSlot<T>>();
};

template<typename T>
auto when_any(std::vector<Task<T>> tasks) {
    return WhenAnyAwaiter<T>{std::move(tasks)};
}

/// Awaitable that resolves when ANY of the input task FACTORIES
/// completes (success or error). Each factory receives its own
/// CancellationToken; when a winner is established, every loser's
/// token is fired so cooperative inner work unwinds promptly instead
/// of running to completion in the background.
///
/// Note: cancellation is best-effort. Inner work that ignores its
/// token still runs to completion off-stage; only its result and
/// exception are dropped.
template<typename T>
class WhenAnyCancellableAwaiter {
public:
    using Factory = std::function<Task<T>(CancellationToken)>;
    using Result  = typename WhenAnyAwaiter<T>::Result;

    explicit WhenAnyCancellableAwaiter(std::vector<Factory> factories)
        : factories_(std::move(factories)) {
        sources_->reserve(factories_.size());
        for (std::size_t i = 0; i < factories_.size(); ++i) {
            sources_->push_back(std::make_shared<CancellationSource>());
        }
    }

    bool await_ready() const noexcept {
        if (factories_.empty()) return true;
        return slot_->winner.load(std::memory_order_acquire) != 0;
    }

    bool await_suspend(std::coroutine_handle<> caller) noexcept {
        detail::publish_race_trace(detail::race_source::kWhenAnyCancellable,
                                   detail::race_op::kStart,
                                   factories_.size());
        for (std::size_t i = 0; i < factories_.size(); ++i) {
            detail::drive_any_cancellable_<T, Factory>(
                std::move(factories_[i]), i, slot_, (*sources_)[i], sources_)
                .start_detached();
        }
        std::lock_guard lk(slot_->mu);
        if (slot_->winner.load(std::memory_order_acquire) != 0) {
            return false;
        }
        slot_->parent_handle = caller;
        slot_->parent_stored = true;
        return true;
    }

    Result await_resume() {
        if (!factories_.empty()) {
            detail::publish_race_trace(detail::race_source::kWhenAnyCancellable,
                                       detail::race_op::kEnd);
        }
        Result r;
        r.index = slot_->winner_index;
        auto& v = slot_->result;
        if (v.index() == 2) {
            r.error = std::get<2>(v);
        } else if constexpr (!std::is_void_v<T>) {
            if (v.index() == 1) r.value = std::get<1>(std::move(v));
        }
        return r;
    }

private:
    std::vector<Factory>                 factories_;
    std::shared_ptr<detail::RaceSlot<T>> slot_ =
        std::make_shared<detail::RaceSlot<T>>();
    std::shared_ptr<std::vector<std::shared_ptr<CancellationSource>>>
        sources_ = std::make_shared<std::vector<std::shared_ptr<CancellationSource>>>();
};

template<typename T>
auto when_any_cancellable(
    std::vector<std::function<Task<T>(CancellationToken)>> factories) {
    return WhenAnyCancellableAwaiter<T>{std::move(factories)};
}

}  // namespace aria::async
