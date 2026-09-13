#pragma once

// QtDispatcher — asynchronous dispatch through a QObject owned by Qt's event
// loop. Construct on the context's thread. Posting and releasing the dispatcher
// are safe from other threads; destroying either the dispatcher or its context
// cancels pending work and releases callback captures immediately.

#include "aria/callback_boundary.hpp"
#include "aria/runtime/dispatcher.hpp"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace aria::adapters::qt6 {

class QtDispatcher final : public runtime::IDispatcher {
    struct Job {
        std::function<void()> callback;
        std::chrono::steady_clock::time_point posted = std::chrono::steady_clock::now();
        std::chrono::milliseconds delay;
    };
    struct State {
        std::mutex mutex;
        QObject* anchor = nullptr;
        bool active = true;
        std::uint64_t next_id = 0;
        std::unordered_map<std::uint64_t, std::shared_ptr<Job>> pending;
    };

    static void close_(const std::shared_ptr<State>& state, bool delete_anchor) noexcept {
        decltype(state->pending) retired;
        {
            std::lock_guard lock(state->mutex);
            state->active = false;
            retired.swap(state->pending);
            if (delete_anchor && state->anchor) {
                // The anchor's destructor takes the same gate, so its address
                // stays live until this queued deletion request is registered.
                QMetaObject::invokeMethod(state->anchor, &QObject::deleteLater,
                                          Qt::QueuedConnection);
            } else if (!delete_anchor) {
                state->anchor = nullptr;
            }
        }
        // Captures may re-enter the dispatcher or destroy other Qt objects.
        // Release them outside the gate, independently of deleteLater delivery.
    }

    class Anchor final : public QObject {
    public:
        Anchor(QObject* parent, std::shared_ptr<State> state)
            : QObject(parent), state_(std::move(state)) {}
        ~Anchor() override { close_(state_, false); }
    private:
        std::shared_ptr<State> state_;
    };

public:
    /// The context controls the owner thread and bounds task lifetime.
    /// An application object is the usual context. A null context or creation
    /// on another thread is rejected instead of silently losing posted work.
    explicit QtDispatcher(QObject* context = QCoreApplication::instance())
        : state_(std::make_shared<State>()) {
        if (!context) throw std::invalid_argument("QtDispatcher: context must not be null");
        if (QThread::currentThread() != context->thread())
            throw std::logic_error("QtDispatcher: construct on the context's owner thread");
        state_->anchor = new Anchor(context, state_);
    }

    ~QtDispatcher() override { close_(state_, true); }
    QtDispatcher(const QtDispatcher&) = delete;
    QtDispatcher& operator=(const QtDispatcher&) = delete;

    void post(std::function<void()> callback) override {
        enqueue_(std::move(callback), std::chrono::milliseconds{0});
    }

    /// Negative delays behave like post(). Large delays are split into Qt
    /// timer intervals and checked against steady_clock, without narrowing
    /// the requested duration or overflowing an absolute clock deadline.
    void post_delayed(std::chrono::milliseconds delay,
                      std::function<void()> callback) override {
        if (delay.count() < 0) delay = std::chrono::milliseconds{0};
        enqueue_(std::move(callback), delay);
    }

    [[nodiscard]] bool is_main_thread() const noexcept override {
        std::lock_guard lock(state_->mutex);
        return state_->active && state_->anchor &&
               QThread::currentThread() == state_->anchor->thread();
    }

    [[nodiscard]] ::aria::SchedulerCaps caps() const noexcept override {
        return ::aria::SchedulerCaps::Post
             | ::aria::SchedulerCaps::Delay
             | ::aria::SchedulerCaps::MainThread
             | ::aria::SchedulerCaps::Autonomous;
    }

private:
    void enqueue_(std::function<void()> callback, std::chrono::milliseconds delay) {
        if (!callback) return;
        // Keep an independent owner outside the lock even if map allocation
        // fails, so unwinding cannot destroy the callback under the gate.
        auto job = std::make_shared<Job>(Job{std::move(callback),
                                            std::chrono::steady_clock::now(), delay});
        auto state = state_;
        std::lock_guard lock(state->mutex);
        if (!state->active || !state->anchor) return;
        if (state->next_id == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("QtDispatcher: task identifier exhausted");
        const auto id = ++state->next_id;
        state->pending.emplace(id, job);
        try {
            if (!QMetaObject::invokeMethod(state->anchor,
                    [weak = std::weak_ptr<State>(state), id] { deliver_(weak, id); },
                    Qt::QueuedConnection)) {
                state->pending.erase(id); // job above still owns the capture
            }
        } catch (...) {
            state->pending.erase(id);
            throw;
        }
    }

    static void deliver_(const std::weak_ptr<State>& weak, std::uint64_t id) noexcept {
        auto state = weak.lock();
        if (!state) return;
        std::shared_ptr<Job> ready;
        try {
            QObject* anchor;
            std::chrono::milliseconds remaining{0};
            {
                std::lock_guard lock(state->mutex);
                if (!state->active || !state->anchor) return;
                auto it = state->pending.find(id);
                if (it == state->pending.end()) return;
                anchor = state->anchor;
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - it->second->posted);
                if (elapsed < it->second->delay) {
                    remaining = it->second->delay - elapsed;
                } else {
                    ready = std::move(it->second);
                    state->pending.erase(it);
                }
            }
            if (ready) {
                // No dispatcher object or mutex is needed while invoking user
                // code; the callback may destroy its dispatcher synchronously.
                ready->callback();
                return;
            }
            // This path runs only on the anchor's owner thread. Off-thread
            // shutdown can invalidate work immediately but deletes the QObject
            // through its event loop, after this invocation has returned.
            auto timer = std::make_unique<QTimer>(anchor);
            timer->setSingleShot(true);
            const auto maximum = std::numeric_limits<int>::max();
            const int interval = remaining.count() > maximum
                ? maximum : static_cast<int>(remaining.count());
            auto* raw = timer.get();
            QObject::connect(raw, &QTimer::timeout, anchor, [weak, id, raw] {
                raw->deleteLater();
                deliver_(weak, id);
            });
            timer->start(interval);
            (void)timer.release(); // QObject parent now owns the timer
        } catch (...) {
            std::shared_ptr<Job> retired;
            {
                std::lock_guard lock(state->mutex);
                if (auto it = state->pending.find(id); it != state->pending.end()) {
                    retired = std::move(it->second);
                    state->pending.erase(it);
                }
            }
            ::aria::report_callback_failure("qt.dispatcher", std::current_exception());
        }
    }

    std::shared_ptr<State> state_;
};

}  // namespace aria::adapters::qt6
