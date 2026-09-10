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
// `send` suspends if the buffer is full and no receiver is waiting; `recv`
// suspends if neither a buffered value nor a sender is available. Capacity
// zero is a rendezvous: each sender waits for a receiver (or vice versa).
// `close()` wakes ALL pending waiters: unassigned receivers observe
// `std::nullopt` (end of stream), and parked senders are released with their
// pending value dropped. Buffered and already assigned values remain readable.
//
// All resumes happen *outside* the internal mutex to avoid recursive locks.
// Multiple producers and consumers may use the channel concurrently. The
// channel must outlive concurrent method calls, and parked coroutine frames
// must remain alive until resumed. Destroying a suspended Task is not a
// cancellation operation.

#include "aria/async/task.hpp"

#include <coroutine>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace aria::async {

template<typename T>
class Channel {
public:
    explicit Channel(std::size_t capacity = std::size_t(-1)) : cap_(capacity) {}

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    Channel(Channel&&) = delete;
    Channel& operator=(Channel&&) = delete;

    /// Releases every still-parked sender / receiver.
    ///
    /// Destroying a channel that still has waiters would otherwise leak one
    /// coroutine frame per waiter: nothing else holds those handles, so they
    /// could never be resumed or destroyed. `close()` is idempotent and
    /// resumes outside the lock, so calling it here is safe even when the
    /// user already closed the channel explicitly.
    ///
    /// NOTE: the resumed coroutines run during this destructor. They must
    /// not start another operation on the channel. A receiver's await_resume
    /// only uses its own delivery slot, so completing a parked receive does
    /// not access the channel again.
    ~Channel() {
        try {
            close();
        } catch (...) {
            // Resuming a waiter must not throw out of a destructor.
        }
    }

    // ── send ──────────────────────────────────────────────────
    auto send(T value) {
        struct Awaiter {
            Channel* self;
            T value;

            bool await_ready() const noexcept { return false; }

            // Reserve delivery for a waiting receiver before considering
            // the buffer. Publish its value before resuming outside the lock.
            bool await_suspend(std::coroutine_handle<> h) noexcept {
                std::coroutine_handle<> wake;
                bool suspend = false;
                {
                    std::lock_guard lk(self->mu_);
                    if (self->closed_) {
                        // Drop the value; just continue.
                        return false;
                    }
                    if (!self->recv_waiters_.empty()) {
                        auto receiver = self->recv_waiters_.front();
                        receiver.value->emplace(std::move(value));
                        self->recv_waiters_.pop_front();
                        wake = receiver.handle;
                    } else if (self->buffer_.size() < self->cap_) {
                        self->buffer_.push_back(std::move(value));
                    } else {
                        self->send_waiters_.push_back({h, std::move(value)});
                        suspend = true;
                    }
                }
                if (wake) wake.resume();  // outside the lock
                return suspend;
            }

            void await_resume() noexcept {}
        };
        return Awaiter{this, std::move(value)};
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
    // `await_ready` always returns false. The authoritative
    // "consume-or-suspend" decision is made entirely inside
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
                    } else if (!self->send_waiters_.empty()) {
                        // Direct rendezvous, including capacity zero.
                        auto sender = std::move(self->send_waiters_.front());
                        self->send_waiters_.pop_front();
                        value.emplace(std::move(sender.value));
                        wake = sender.handle;
                        suspend = false;
                    } else if (self->closed_) {
                        // value stays empty → resume with std::nullopt.
                        suspend = false;
                    } else {
                        // Genuinely empty and open: register atomically.
                        self->recv_waiters_.push_back({h, &value});
                        suspend = true;
                    }
                }
                if (wake) wake.resume();  // outside the lock
                return suspend;
            }

            std::optional<T> await_resume() noexcept {
                // This receive owns its value before it is resumed. Never
                // race another consumer for a value in the shared buffer.
                return std::move(value);
            }
        };
        return Awaiter{this, std::nullopt};
    }

    /// Mark the channel closed and release EVERY parked coroutine.
    ///
    /// Still-parked receivers observe an empty `optional` (end of stream).
    /// Values already buffered or assigned to a receiver are not discarded.
    /// Senders parked on a full buffer are woken too and their pending
    /// values are dropped — matching `send()`'s own "channel already
    /// closed → drop the value and continue" behaviour. Waking only the
    /// receivers (the original implementation) left every blocked sender
    /// suspended forever, leaking one coroutine frame each and hanging any
    /// producer that was waiting for buffer space at close time.
    void close() {
        std::deque<PendingReceive> receivers;
        std::deque<PendingSend> senders;
        {
            std::lock_guard lk(mu_);
            closed_ = true;
            receivers.swap(recv_waiters_);
            senders.swap(send_waiters_);
        }
        for (auto receiver : receivers)
            receiver.handle.resume();
        for (auto& sender : senders)
            sender.handle.resume();
        // Pending send values are destroyed here, outside the channel mutex.
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
    struct PendingReceive {
        std::coroutine_handle<> handle;
        std::optional<T>* value;
    };

    struct PendingSend {
        std::coroutine_handle<> handle;
        T value;
    };

    mutable std::mutex mu_;
    std::deque<T> buffer_;
    std::deque<PendingReceive> recv_waiters_;
    std::deque<PendingSend> send_waiters_;
    std::size_t cap_;
    bool closed_ = false;
};

}  // namespace aria::async
