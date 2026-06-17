#pragma once

#include "aria/abi/export.hpp"
#include "aria/property_ops.hpp"  // IDelayedScheduler
#include <chrono>
#include <functional>
#include <memory>
#include <cstddef>

namespace aria::runtime {

/// Abstract main-thread dispatcher.
///
/// Posts callables to be executed on the platform's main / UI thread.
/// Concrete implementations exist for each platform (AppKit/UIKit/JNI/Qt/Emscripten),
/// plus a generic in-process implementation for testing and console apps.
///
/// Inherits from `IDelayedScheduler` (and therefore from `aria::IScheduler`)
/// so it can be used directly by `debounce` / `throttle` /
/// `retry_with_backoff` / unified `IScheduler` consumers without an adapter.
///
/// Capability bitmask is `Post | Delay | MainThread | Pumpable`. Concrete
/// platform dispatchers may widen it (e.g. by adding `GraphSafe`/`WorkerSafe`
/// in `caps()` overrides) — by default we conservatively claim the four
/// universal capabilities.
class ARIA_RUNTIME_API IDispatcher : public ::aria::IDelayedScheduler {
public:
    ~IDispatcher() override = default;

    /// Schedule the callable to run on the main thread (asynchronously).
    virtual void post(std::function<void()> fn) = 0;

    /// Like post(), but with a delay.
    virtual void post_delayed(std::chrono::milliseconds delay, std::function<void()> fn) = 0;

    /// Returns true if currently executing on the main thread.
    [[nodiscard]] bool is_main_thread() const noexcept override = 0;

    /// IDelayedScheduler adapter — routes to `post_delayed`.
    void post_after(std::chrono::milliseconds delay,
                    std::function<void()> fn) override {
        post_delayed(delay, std::move(fn));
    }

    // ── IScheduler bridge ────────────────────────────────────────────
    [[nodiscard]] ::aria::SchedulerCaps caps() const noexcept override {
        return ::aria::SchedulerCaps::Post
             | ::aria::SchedulerCaps::Delay
             | ::aria::SchedulerCaps::MainThread
             | ::aria::SchedulerCaps::Pumpable;
    }
    void schedule(std::function<void()> fn) override {
        post(std::move(fn));
    }
    void schedule_after(std::chrono::milliseconds delay,
                        std::function<void()> fn) override {
        post_delayed(delay, std::move(fn));
    }
};

/// Global accessor — set once at startup by your platform integration code.
/// (Platform-specific adapters install their dispatcher here.)
ARIA_RUNTIME_API IDispatcher& main_dispatcher();
ARIA_RUNTIME_API void set_main_dispatcher(std::shared_ptr<IDispatcher> dispatcher);

/// In-process dispatcher: runs queued callables when `run()` is invoked
/// (typically from your main loop). Suitable for console apps and tests.
class ARIA_RUNTIME_API SimpleDispatcher : public IDispatcher {
public:
    SimpleDispatcher();
    ~SimpleDispatcher() override;

    SimpleDispatcher(const SimpleDispatcher&) = delete;
    SimpleDispatcher& operator=(const SimpleDispatcher&) = delete;
    SimpleDispatcher(SimpleDispatcher&&) = delete;
    SimpleDispatcher& operator=(SimpleDispatcher&&) = delete;

    void post(std::function<void()> fn) override;
    void post_delayed(std::chrono::milliseconds delay, std::function<void()> fn) override;
    [[nodiscard]] bool is_main_thread() const noexcept override;

    /// Pump pending callables. Returns the number processed.
    std::size_t pump(std::chrono::milliseconds budget = std::chrono::milliseconds{50});

    /// Block until at least one callable is available, then pump it.
    void run_one();

private:
    struct Impl;
    // RAII pImpl; C4251 on the unique_ptr member is a false positive for an
    // incomplete opaque pointee consumed only via non-template API.
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif
    std::unique_ptr<Impl> impl_;
#ifdef _MSC_VER
#  pragma warning(pop)
#endif
};

}  // namespace aria::runtime
