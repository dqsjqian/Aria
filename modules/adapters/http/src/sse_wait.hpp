#pragma once

#include <mira/core/event_loop.hpp>
#include <mutex>
#include <stop_token>

namespace aria::adapters::http::detail {

// Register parked SSE consumers with the event loop, so cancelling the server's
// other I/O cannot make the loop appear idle before close() wakes the consumers.
// Client supplies mu, wake_source and ready_locked(). The mutex makes checking
// the outbox and publishing the wake source atomic with push()/close(). A stop
// requested before sleep registration remains observable, so no wake is lost.
template<class Client>
Mira::Task<void> wait_for_sse(Client* client, Mira::EventLoop& loop) {
    std::stop_token wake;
    {
        std::lock_guard lock(client->mu);
        if (client->ready_locked()) co_return;
        client->wake_source = std::stop_source{};
        wake = client->wake_source.get_token();
    }
    (void)co_await loop.sleep_until(Mira::EventLoop::Clock::time_point::max(),
                                  {.stop = wake});
}

}  // namespace aria::adapters::http::detail
