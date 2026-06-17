#include <doctest/doctest.h>

#include "aria/async/channel.hpp"
#include "aria/async/executor.hpp"
#include "aria/async/task.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace aria::async;

TEST_CASE("Channel: synchronous size/close API") {
    Channel<int> ch{4};
    CHECK(ch.size() == 0);
    CHECK_FALSE(ch.is_closed());
    ch.close();
    CHECK(ch.is_closed());
}

namespace {
// Free functions instead of lambdas to keep coroutine state safe.
Task<void> producer_task(Channel<int>& ch) {
    for (int i = 1; i <= 5; ++i) co_await ch.send(i);
    ch.close();
}
Task<void> consumer_task(Channel<int>& ch, std::atomic<int>& total,
                         std::atomic<bool>& done) {
    while (true) {
        auto v = co_await ch.recv();
        if (!v) break;
        total.fetch_add(*v);
    }
    done = true;
}
Task<void> waiting_consumer(Channel<int>& ch,
                            std::atomic<bool>& got_empty,
                            std::atomic<bool>& done) {
    auto v = co_await ch.recv();
    if (!v) got_empty = true;
    done = true;
}
}  // namespace

TEST_CASE("Channel: producer/consumer (single-threaded coroutines)") {
    Channel<int> ch{8};
    std::atomic<int> total{0};
    std::atomic<bool> done{false};

    // Start consumer first (it will suspend on empty buffer)
    std::move(consumer_task(ch, total, done)).start_detached();
    // Producer pushes 5 items + close — runs synchronously to completion
    std::move(producer_task(ch)).start_detached();

    // Both run on the calling thread (channel resumes inline).
    CHECK(done.load());
    CHECK(total.load() == 1 + 2 + 3 + 4 + 5);
}

TEST_CASE("Channel: close wakes pending recv with empty optional") {
    Channel<int> ch{2};
    std::atomic<bool> got_empty{false};
    std::atomic<bool> done{false};

    std::move(waiting_consumer(ch, got_empty, done)).start_detached();
    // Consumer is now suspended on empty buffer.
    CHECK_FALSE(done.load());
    ch.close();
    // close() resumes the consumer synchronously
    CHECK(done.load());
    CHECK(got_empty.load());
}
