#pragma once

// Channel<T>: bounded async queue between coroutines.
//
//   Channel<int> ch{capacity = 4};
//
//   // Producer
//   Task<void> producer() {
//       for (int i = 0; i < 10; ++i) co_await ch.send(i);
//       ch.close();
//   }
//
//   // Consumer
//   Task<void> consumer() {
//       while (auto v = co_await ch.recv()) {
//           std::cout << *v << '\n';
//       }
//   }
//
// `send` suspends if the buffer is full; `recv` suspends if it is empty.
// `close()` wakes all pending receivers with std::nullopt.
//
// All resumes happen *outside* the internal mutex to avoid recursive locks.

#include "aria/async/task.hpp"

#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace aria::async {

template<typename T>
class Channel {
public:
    explicit Channel(std::size_t capacity = std::size_t(-1)) : cap_(capacity) {}

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    // ── send ──────────────────────────────────────────────────
    auto send(T value) {
        struct Awaiter {
            Channel* self;
            T value;
            std::coroutine_handle<> waiter_to_wake{};

            bool await_ready() const noexcept { return false; }

            // Try to push synchronously; if full, suspend.
            // If we pushed and there is a recv-waiter, hand it back so
            // await_resume can wake it AFTER unlocking.
            bool await_suspend(std::coroutine_handle<> h) noexcept {
                std::coroutine_handle<> wake;
                bool suspend = false;
                {
                    std::lock_guard lk(self->mu_);
                    if (self->closed_) {
                        // Drop the value; just continue.
                        return false;
                    }
                    if (self->buffer_.size() < self->cap_) {
                        self->buffer_.push_back(std::move(value));
                        if (!self->recv_waiters_.empty()) {
                            wake = self->recv_waiters_.front();
                            self->recv_waiters_.pop_front();
                        }
                    } else {
                        self->send_waiters_.push_back({h, std::move(value)});
                        suspend = true;
                    }
                }
                if (wake) wake.resume();   // outside the lock
                return suspend;
            }

            void await_resume() noexcept {}
        };
        return Awaiter{this, std::move(value), {}};
    }

    // ── recv ──────────────────────────────────────────────────
    //
    // Lost-wakeup safety
    // ------------------
    // The decision "is there a value right now?" and "register myself as a
    // waiter" MUST be atomic with respect to a concurrent `send()` / `close()`.
    // An earlier design consumed the value in `await_ready` (taking the lock)
    // and, if empty, registered the waiter in a SEPARATE `await_suspend` lock
    // acquisition. Between those two locks a `send()` could push to the
    // buffer, find `recv_waiters_` still empty, and return WITHOUT waking
    // anyone — the receiver then parked forever (until the next send/close).
    //
    // Fix: `await_ready` only reports the fast path (a value already sitting
    // in the buffer with no contention window of consequence). The
    // authoritative "consume-or-suspend" decision is made entirely inside
    // `await_suspend` under a single lock hold: if a value/closed state is
    // observable there, we consume it and return `false` (resume without
    // parking); otherwise we register the waiter and return `true`. There is
    // no longer any gap between the empty-check and the registration.
    auto recv() {
        struct Awaiter {
            Channel* self;
            std::optional<T> value{};

            // Always go through await_suspend so the consume/register
            // decision is made atomically under one lock. Returning false
            // here keeps the awaiter cheap; the real work is in
            // await_suspend.
            bool await_ready() noexcept { return false; }

            // Returns false (do not suspend) if a value was consumed or the
            // channel is closed; true (suspend) once the handle is registered.
            bool await_suspend(std::coroutine_handle<> h) noexcept {
                std::coroutine_handle<> wake;
                bool suspend;
                {
                    std::lock_guard lk(self->mu_);
                    if (!self->buffer_.empty()) {
                        value.emplace(std::move(self->buffer_.front()));
                        self->buffer_.pop_front();
                        // A blocked sender can now deposit its value.
                        if (!self->send_waiters_.empty()) {
                            auto w = std::move(self->send_waiters_.front());
                            self->send_waiters_.pop_front();
                            self->buffer_.push_back(std::move(w.value));
                            wake = w.handle;
                        }
                        suspend = false;
                    } else if (self->closed_) {
                        // value stays empty → resume with std::nullopt.
                        suspend = false;
                    } else {
                        // Genuinely empty and open: register atomically.
                        self->recv_waiters_.push_back(h);
                        suspend = true;
                    }
                }
                if (wake) wake.resume();   // outside the lock
                return suspend;
            }

            std::optional<T> await_resume() noexcept {
                // Fast path: value already consumed in await_suspend.
                if (value.has_value()) return std::move(value);
                // We were parked and later resumed by a producer push or by
                // close(). Re-acquire and consume whatever is now available.
                std::optional<T> out;
                std::coroutine_handle<> wake;
                {
                    std::lock_guard lk(self->mu_);
                    if (!self->buffer_.empty()) {
                        out.emplace(std::move(self->buffer_.front()));
                        self->buffer_.pop_front();
                        if (!self->send_waiters_.empty()) {
                            auto w = std::move(self->send_waiters_.front());
                            self->send_waiters_.pop_front();
                            self->buffer_.push_back(std::move(w.value));
                            wake = w.handle;
                        }
                    }
                    // else: closed → out stays empty
                }
                if (wake) wake.resume();
                return out;
            }
        };
        return Awaiter{this, std::nullopt};
    }

    /// Mark the channel closed. Any pending recv() awaiters wake up with empty.
    void close() {
        std::vector<std::coroutine_handle<>> to_wake;
        {
            std::lock_guard lk(mu_);
            closed_ = true;
            while (!recv_waiters_.empty()) {
                to_wake.push_back(recv_waiters_.front());
                recv_waiters_.pop_front();
            }
        }
        for (auto h : to_wake) h.resume();
    }

    [[nodiscard]] bool is_closed() const {
        std::lock_guard lk(mu_);
        return closed_;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lk(mu_);
        return buffer_.size();
    }

private:
    struct PendingSend {
        std::coroutine_handle<> handle;
        T value;
    };

    mutable std::mutex mu_;
    std::deque<T> buffer_;
    std::deque<std::coroutine_handle<>> recv_waiters_;
    std::deque<PendingSend> send_waiters_;
    std::size_t cap_;
    bool closed_ = false;
};

}  // namespace aria::async
