#pragma once

// ViewModelScope — wires CoroutineScope into a ViewModel so that destroying
// the VM cancels every coroutine launched into it AND waits for them to
// exit before VM teardown returns.
//
// Usage (inside a ViewModel subclass):
//
//   class MyVm : public ViewModel {
//   public:
//       MyVm() { scope_.attach(*this); }
//
//       void start_polling() {
//           scope_.launch([this](async::CancellationToken tok) -> async::Task<void> {
//               while (!tok.is_cancelled()) {
//                   co_await schedule_after(timer_, 1s);
//                   tok.throw_if_cancelled();
//                   poll_data();
//               }
//           });
//       }
//   private:
//       binding::ViewModelScope scope_;
//   };
//
// Declare the scope after the VM members used by its work (members are
// destroyed in reverse order). Its destructor cancels and joins before those
// members are torn down. If work touches state destroyed by a custom VM
// destructor body, call cancel_and_join() at the start of that body.
//
// When MyVm is destroyed: the scope calls
// `cancel_and_join()` (bounded by `kJoinTimeoutMs`, default 5 s). It
// cancels the source synchronously, then blocks until every wrapper
// coroutine has decremented the inflight counter to zero. If any
// coroutine is stuck on a non-cancellable await, the leak is reported
// through the async error sink (see <aria/async/async_error_sink.hpp>)
// and teardown returns rather than blocking process shutdown
// indefinitely.

#include "aria/async/cancellation.hpp"
#include "aria/async/scope.hpp"
#include "aria/binding/view_model.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>

namespace aria::binding {

class ViewModelScope {
public:
    /// Default timeout for `cancel_and_join()` invoked from the VM's
    /// destroy-hook. Matches the underlying `CoroutineScope` default
    /// and is generous enough for cooperatively-cancelled work; a
    /// stuck coroutine is reported as a leak rather than blocking
    /// process shutdown.
    static constexpr std::chrono::milliseconds kJoinTimeoutMs{5000};

    ViewModelScope() : state_(std::make_shared<State>()) {}

    ~ViewModelScope() { shutdown_(*state_); }
    ViewModelScope(const ViewModelScope&) = delete;
    ViewModelScope& operator=(const ViewModelScope&) = delete;
    ViewModelScope(ViewModelScope&&) = delete;
    ViewModelScope& operator=(ViewModelScope&&) = delete;

    /// Tie this scope's lifetime to the ViewModel's destructor.
    /// Must be called from the VM's ctor body.
    ///
    /// The hook also covers scopes owned outside the ViewModel. A scope
    /// member already cancels in its own destructor; attaching it remains
    /// harmless because cancellation and joining are idempotent.
    void attach(ViewModel& vm) {
        auto keep = state_;
        vm.add_destroy_hook([keep]() noexcept {
            // Structured-concurrency boundary: cancel + wait. If any
            // coroutine is stuck, CoroutineScope reports the leak
            // through the async error sink (see scope.hpp).
            shutdown_(*keep);
        });
    }

    [[nodiscard]] async::CancellationToken token() const noexcept {
        return state_->scope.token();
    }
    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_->scope.is_cancelled();
    }
    [[nodiscard]] std::size_t inflight_count() const noexcept {
        return state_->scope.inflight_count();
    }

    /// Request cancellation only (non-blocking).
    void cancel() noexcept { state_->scope.cancel(); }

    /// Cancel + synchronously wait for all in-flight coroutines to
    /// exit, with the given timeout. Returns true on full drain.
    bool cancel_and_join(std::chrono::milliseconds timeout = kJoinTimeoutMs) noexcept {
        return state_->scope.cancel_and_join(timeout);
    }

    template<typename Fn>
    void launch(Fn factory) { state_->scope.launch(std::move(factory)); }

    void launch_simple(async::Task<void> task) {
        state_->scope.launch_simple(std::move(task));
    }

private:
    struct State {
        async::CoroutineScope scope;
        bool teardown_started = false;
    };
    static void shutdown_(State& state) noexcept {
        if (std::exchange(state.teardown_started, true)) return;
        state.scope.cancel_and_join(kJoinTimeoutMs);
    }
    std::shared_ptr<State> state_;
};

}  // namespace aria::binding
