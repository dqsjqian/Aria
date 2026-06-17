#pragma once
//
// Executors.h — adapters that let Aria's coroutine / operator layer
// target a runtime::IDispatcher (e.g. the Qt main thread).
//
// Very trivial forwarders — inlined directly.
//

#include "aria/async/executor.hpp"
#include "aria/property_ops.hpp"
#include "aria/runtime/dispatcher.hpp"

#include <chrono>
#include <functional>
#include <utility>

namespace showcase::app {

/// IExecutor forwarding post() to a runtime::IDispatcher.
/// Use case: UI executor for AsyncCommand.
class DispatcherExec final : public aria::async::IExecutor {
public:
    explicit DispatcherExec(aria::runtime::IDispatcher& d) noexcept : d_(d) {}
    void post(std::function<void()> fn) override { d_.post(std::move(fn)); }
private:
    aria::runtime::IDispatcher& d_;
};

/// IDelayedScheduler backed by IDispatcher::post_delayed.
/// Use case: reactive operators (debounce / throttle).
class DispatcherDelay final : public aria::IDelayedScheduler {
public:
    explicit DispatcherDelay(aria::runtime::IDispatcher& d) noexcept : d_(d) {}
    void post_after(std::chrono::milliseconds delay,
                    std::function<void()> fn) override {
        d_.post_delayed(delay, std::move(fn));
    }
private:
    aria::runtime::IDispatcher& d_;
};

}  // namespace showcase::app
