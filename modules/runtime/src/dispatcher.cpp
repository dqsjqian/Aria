#include "aria/runtime/dispatcher.hpp"
#include "aria/callback_boundary.hpp"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>

namespace aria::runtime {
namespace {
using Clock = std::chrono::steady_clock;

Clock::time_point deadline_after(std::chrono::milliseconds delay) noexcept {
    const auto now = Clock::now();
    if (delay <= std::chrono::milliseconds::zero()) return now;
    if (delay >= std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max()))
        return Clock::time_point::max();
    const auto span = std::chrono::duration_cast<Clock::duration>(delay);
    return now > Clock::time_point::max() - span ? Clock::time_point::max() : now + span;
}

struct DelayedItem {
    Clock::time_point ready_at;
    // Moving out the callable does not change the heap ordering.
    mutable std::function<void()> fn;
    bool operator<(const DelayedItem& other) const noexcept {
        return ready_at > other.ready_at;
    }
};

struct GlobalDispatcher {
    std::mutex mutex;
    std::shared_ptr<IDispatcher> dispatcher;
};

GlobalDispatcher& global_dispatcher() {
    static GlobalDispatcher state;
    return state;
}
}  // namespace

struct SimpleDispatcher::Impl {
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<std::function<void()>> queue;
    std::priority_queue<DelayedItem> delayed;
    const std::thread::id owner = std::this_thread::get_id();
    bool closed = false;

    // Called under mutex. No allocation or user-code invocation.
    std::function<void()> take_ready() {
        if (!queue.empty()) {
            auto fn = std::move(queue.front());
            queue.pop();
            return fn;
        }
        if (!delayed.empty() && delayed.top().ready_at <= Clock::now()) {
            auto fn = std::move(delayed.top().fn);
            delayed.pop();
            return fn;
        }
        return {};
    }

    void require_owner() const {
        if (std::this_thread::get_id() != owner)
            throw std::logic_error("SimpleDispatcher must be pumped on its creating thread");
    }
};

SimpleDispatcher::SimpleDispatcher() : impl_(std::make_shared<Impl>()) {}

SimpleDispatcher::~SimpleDispatcher() {
    const auto state = impl_;
    {
        std::lock_guard lock(state->mutex);
        state->closed = true;
    }
    state->cv.notify_all();
    // Destroy captures outside the lock while the closed state remains valid.
    // A capture destructor may post again; closed dispatchers discard that work.
    for (;;) {
        std::function<void()> discarded;
        {
            std::lock_guard lock(state->mutex);
            if (!state->queue.empty()) {
                discarded = std::move(state->queue.front());
                state->queue.pop();
            } else if (!state->delayed.empty()) {
                discarded = std::move(state->delayed.top().fn);
                state->delayed.pop();
            } else {
                break;
            }
        }
    }
}

void SimpleDispatcher::post(std::function<void()> fn) {
    if (!fn) return;
    const auto state = impl_;
    {
        std::lock_guard lock(state->mutex);
        if (state->closed) return;
        state->queue.push(std::move(fn));
    }
    state->cv.notify_one();
}

void SimpleDispatcher::post_delayed(std::chrono::milliseconds delay,
                                    std::function<void()> fn) {
    if (!fn) return;
    if (delay <= std::chrono::milliseconds::zero()) {
        post(std::move(fn));
        return;
    }
    const auto state = impl_;
    const auto deadline = deadline_after(delay);
    {
        std::lock_guard lock(state->mutex);
        if (state->closed) return;
        state->delayed.push(DelayedItem{deadline, std::move(fn)});
    }
    state->cv.notify_one();
}

bool SimpleDispatcher::is_main_thread() const noexcept {
    return std::this_thread::get_id() == impl_->owner;
}

std::size_t SimpleDispatcher::pump(std::chrono::milliseconds budget) {
    const auto state = impl_;
    state->require_owner();
    const auto deadline = deadline_after(budget);
    std::size_t count = 0;
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard lock(state->mutex);
            if (state->closed) break;
            fn = state->take_ready();
        }
        if (!fn) break;
        try {
            fn();
        } catch (...) {
            aria::report_callback_failure("runtime.simple_dispatcher.pump",
                                          std::current_exception());
        }
        ++count;
        if (Clock::now() >= deadline) break;
    }
    return count;
}

void SimpleDispatcher::run_one() {
    const auto state = impl_;
    state->require_owner();
    std::function<void()> fn;
    {
        std::unique_lock lock(state->mutex);
        while (!state->closed) {
            fn = state->take_ready();
            if (fn) break;
            if (!state->delayed.empty()) {
                // Copy before releasing the lock: another producer may grow
                // the heap while wait_until still refers to its argument.
                const auto deadline = state->delayed.top().ready_at;
                state->cv.wait_until(lock, deadline);
            } else {
                state->cv.wait(lock);
            }
        }
    }
    if (fn) {
        try {
            fn();
        } catch (...) {
            aria::report_callback_failure("runtime.simple_dispatcher.run_one",
                                          std::current_exception());
        }
    }
}

void set_main_dispatcher(std::shared_ptr<IDispatcher> dispatcher) {
    auto& state = global_dispatcher();
    {
        std::lock_guard lock(state.mutex);
        state.dispatcher.swap(dispatcher);
    }
}

std::shared_ptr<IDispatcher> main_dispatcher() {
    auto& state = global_dispatcher();
    std::lock_guard lock(state.mutex);
    if (!state.dispatcher) state.dispatcher = std::make_shared<SimpleDispatcher>();
    return state.dispatcher;
}

}  // namespace aria::runtime
