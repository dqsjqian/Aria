#pragma once

#include <coroutine>
#include <mutex>

namespace aria::adapters::http::detail {

// Kept independent of the socket transport so the check/park race can be
// exercised deterministically. Client supplies mu, waiter and ready_locked().
template<class Client>
struct SseWait {
    Client* client;

    bool await_ready() const {
        std::lock_guard lock(client->mu);
        return client->ready_locked();
    }

    bool await_suspend(std::coroutine_handle<> waiting) {
        std::lock_guard lock(client->mu);
        // A producer may publish between await_ready and this lock. Returning
        // false resumes only after the lock is released; inline resume here
        // would deadlock when the consumer locks the outbox to drain it.
        if (client->ready_locked()) return false;
        client->waiter = waiting;
        return true;
    }

    void await_resume() const noexcept {}
};

}  // namespace aria::adapters::http::detail
