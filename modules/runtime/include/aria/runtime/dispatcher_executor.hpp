#pragma once
//
// dispatcher_executor.hpp — adapters that bridge IDispatcher to IExecutor
// and IDelayedScheduler. Moved from the Qt demo into the framework so
// every platform host can reuse them without copy-pasting.
//
// Usage:
//   auto exec = std::make_shared<DispatcherExecutor>(my_dispatcher);
//   AsyncCommand<void> cmd{*exec, pool, ...};
//

#include "aria/async/executor.hpp"
#include "aria/property_ops.hpp"
#include "aria/runtime/dispatcher.hpp"

#include <chrono>
#include <functional>
#include <utility>

namespace aria::runtime {

/// IExecutor forwarding post() to a runtime::IDispatcher.
/// Use case: UI executor for AsyncCommand — ensures coroutine resumption
/// happens on the main thread after the worker finishes.
class DispatcherExecutor final : public aria::async::IExecutor {
public:
    explicit DispatcherExecutor(IDispatcher& d) noexcept : d_(d) {}

    void post(std::function<void()> fn) override { d_.post(std::move(fn)); }

    /// Dispatcher-backed executor: safe as graph executor (main-thread affinity),
    /// safe as worker (posts land on the main thread), main-thread + pumpable
    /// characteristics inherited from the underlying dispatcher.
    [[nodiscard]] aria::SchedulerCaps caps() const noexcept override {
        return aria::SchedulerCaps::Post
             | aria::SchedulerCaps::GraphSafe
             | aria::SchedulerCaps::WorkerSafe
             | aria::SchedulerCaps::MainThread
             | aria::SchedulerCaps::Pumpable;
    }

    [[nodiscard]] bool is_main_thread() const noexcept override {
        return d_.is_main_thread();
    }

private:
    IDispatcher& d_;
};

/// IDelayedScheduler backed by IDispatcher::post_delayed.
/// Use case: reactive operators (debounce / throttle) that need a
/// platform-integrated timer rather than a thread-pool sleep.
class DispatcherScheduler final : public aria::IDelayedScheduler {
public:
    explicit DispatcherScheduler(IDispatcher& d) noexcept : d_(d) {}

    void post_after(std::chrono::milliseconds delay,
                    std::function<void()> fn) override {
        d_.post_delayed(delay, std::move(fn));
    }

    [[nodiscard]] bool is_main_thread() const noexcept override {
        return d_.is_main_thread();
    }

    [[nodiscard]] aria::SchedulerCaps caps() const noexcept override {
        return aria::SchedulerCaps::Post
             | aria::SchedulerCaps::Delay
             | aria::SchedulerCaps::MainThread
             | aria::SchedulerCaps::Pumpable;
    }

    void schedule(std::function<void()> fn) override {
        d_.post(std::move(fn));
    }

    void schedule_after(std::chrono::milliseconds delay,
                        std::function<void()> fn) override {
        d_.post_delayed(delay, std::move(fn));
    }

private:
    IDispatcher& d_;
};

}  // namespace aria::runtime
