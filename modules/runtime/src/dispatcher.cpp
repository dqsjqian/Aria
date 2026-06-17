#include "aria/runtime/dispatcher.hpp"
#include <memory>
#include "aria/callback_boundary.hpp"
#include "aria/property_ops.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace aria {
IDelayedScheduler::~IDelayedScheduler() = default;
}  // namespace aria

namespace aria::runtime {

namespace {

struct DelayedItem {
    std::chrono::steady_clock::time_point ready_at;
    // `mutable` so we can move-out the callable from `priority_queue::top()`,
    // whose API only hands out const references. The heap ordering depends
    // solely on `ready_at`, so mutating `fn` does not invalidate the heap.
    mutable std::function<void()> fn;
    bool operator<(const DelayedItem& other) const {
        return ready_at > other.ready_at;  // min-heap
    }
};

std::shared_ptr<IDispatcher>& global_dispatcher_slot() {
    static std::shared_ptr<IDispatcher> slot;
    return slot;
}

/// Guards access to global_dispatcher_slot().  A plain mutex is fine — this
/// is hit at most a handful of times per process (startup + occasional reads).
std::mutex& global_dispatcher_mutex() {
    static std::mutex m;
    return m;
}

}  // namespace

struct SimpleDispatcher::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::function<void()>> queue;
    std::priority_queue<DelayedItem> delayed;
    std::thread::id owner;
};

SimpleDispatcher::SimpleDispatcher()
    : impl_(std::make_unique<Impl>()) {
    impl_->owner = std::this_thread::get_id();
}

SimpleDispatcher::~SimpleDispatcher() = default;

void SimpleDispatcher::post(std::function<void()> fn) {
    {
        std::lock_guard lk(impl_->mutex);
        impl_->queue.push(std::move(fn));
    }
    impl_->cv.notify_one();
}

void SimpleDispatcher::post_delayed(std::chrono::milliseconds delay,
                                    std::function<void()> fn) {
    auto deadline = std::chrono::steady_clock::now() + delay;
    {
        std::lock_guard lk(impl_->mutex);
        impl_->delayed.push(DelayedItem{deadline, std::move(fn)});
    }
    impl_->cv.notify_one();
}

bool SimpleDispatcher::is_main_thread() const noexcept {
    return std::this_thread::get_id() == impl_->owner;
}

std::size_t SimpleDispatcher::pump(std::chrono::milliseconds budget) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    std::size_t count = 0;

    while (true) {
        std::function<void()> fn;
        {
            std::lock_guard lk(impl_->mutex);
            // Move ready delayed items into main queue
            auto now = std::chrono::steady_clock::now();
            while (!impl_->delayed.empty() && impl_->delayed.top().ready_at <= now) {
                impl_->queue.push(std::move(impl_->delayed.top().fn));
                impl_->delayed.pop();
            }

            if (impl_->queue.empty()) break;
            fn = std::move(impl_->queue.front());
            impl_->queue.pop();
        }
        try {
            fn();
        } catch (...) {
            aria::report_callback_failure(
                std::string_view{"runtime.simple_dispatcher.pump"},
                std::current_exception());
        }
        ++count;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
    return count;
}

void SimpleDispatcher::run_one() {
    std::function<void()> fn;
    {
        std::unique_lock lk(impl_->mutex);
        // Loop until something is actually actionable. Two ways to be
        // ready: (a) an item is sitting in `queue`, (b) a delayed item
        // whose deadline has now passed.
        //
        // The earlier implementation had a race: it took the soonest
        // delayed deadline and `cv.wait_until(deadline)`. If a `post()`
        // raced that wait and added a queue task, we'd be notified,
        // re-check `delayed` (still in the future), and return WITHOUT
        // ever looking at `queue`. The just-posted task was silently
        // skipped until the next `run_one` call. Fix: drain ready
        // delayed items first, prefer `queue`, and re-loop after every
        // wake until one path actually has something for us.
        while (true) {
            const auto now = std::chrono::steady_clock::now();
            while (!impl_->delayed.empty() &&
                   impl_->delayed.top().ready_at <= now) {
                impl_->queue.push(std::move(impl_->delayed.top().fn));
                impl_->delayed.pop();
            }
            if (!impl_->queue.empty()) {
                fn = std::move(impl_->queue.front());
                impl_->queue.pop();
                break;
            }
            if (!impl_->delayed.empty()) {
                // Sleep until the soonest deadline OR a post wakes us.
                impl_->cv.wait_until(lk, impl_->delayed.top().ready_at);
            } else {
                // No delayed work either — block until a post arrives.
                impl_->cv.wait(lk);
            }
        }
    }
    if (fn) {
        try {
            fn();
        } catch (...) {
            aria::report_callback_failure(
                std::string_view{"runtime.simple_dispatcher.run_one"},
                std::current_exception());
        }
    }
}

void set_main_dispatcher(std::shared_ptr<IDispatcher> d) {
    std::lock_guard lk(global_dispatcher_mutex());
    global_dispatcher_slot() = std::move(d);
}

IDispatcher& main_dispatcher() {
    std::lock_guard lk(global_dispatcher_mutex());
    auto& slot = global_dispatcher_slot();
    if (!slot) {
        // Auto-install a SimpleDispatcher if none was installed.
        // IMPORTANT: call this once from the thread you consider "main"
        // (typically very early during startup) so SimpleDispatcher captures
        // the correct owner thread id.
        slot = std::make_shared<SimpleDispatcher>();
    }
    return *slot;
}

}  // namespace aria::runtime
