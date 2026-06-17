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
// When MyVm is destroyed: the scope's destroy-hook calls
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

    ViewModelScope() : scope_(std::make_shared<async::CoroutineScope>()) {}

    /// Tie this scope's lifetime to the ViewModel's destructor.
    /// Must be called from the VM's ctor body.
    ///
    /// We capture the scope by *shared* ownership in the destroy-hook so
    /// the cancel-and-join can still happen even when ViewModelScope
    /// itself (a derived-class member) has already been destroyed
    /// before the base ViewModel destructor runs.
    void attach(ViewModel& vm) {
        std::shared_ptr<async::CoroutineScope> keep = scope_;
        vm.add_destroy_hook([keep]() noexcept {
            // Structured-concurrency boundary: cancel + wait. If any
            // coroutine is stuck, CoroutineScope reports the leak
            // through the async error sink (see scope.hpp).
            keep->cancel_and_join(kJoinTimeoutMs);
        });
    }

    [[nodiscard]] async::CancellationToken token() const noexcept {
        return scope_->token();
    }
    [[nodiscard]] bool is_cancelled() const noexcept {
        return scope_->is_cancelled();
    }
    [[nodiscard]] std::size_t inflight_count() const noexcept {
        return scope_->inflight_count();
    }

    /// Request cancellation only (non-blocking).
    void cancel() noexcept { scope_->cancel(); }

    /// Cancel + synchronously wait for all in-flight coroutines to
    /// exit, with the given timeout. Returns true on full drain.
    bool cancel_and_join(std::chrono::milliseconds timeout = kJoinTimeoutMs) noexcept {
        return scope_->cancel_and_join(timeout);
    }

    template<typename Fn>
    void launch(Fn factory) { scope_->launch(std::move(factory)); }

    void launch_simple(async::Task<void> task) {
        scope_->launch_simple(std::move(task));
    }

private:
    std::shared_ptr<async::CoroutineScope> scope_;
};

}  // namespace aria::binding
