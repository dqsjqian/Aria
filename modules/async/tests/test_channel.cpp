#include "aria/async/channel.hpp"
#include "aria/async/task.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <doctest/doctest.h>
#include <memory>
#include <semaphore>
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
    for (int i = 1; i <= 5; ++i)
        co_await ch.send(i);
    ch.close();
}
Task<void> consumer_task(Channel<int>& ch, std::atomic<int>& total, std::atomic<bool>& done) {
    while (true) {
        auto v = co_await ch.recv();
        if (!v) break;
        total.fetch_add(*v);
    }
    done = true;
}
Task<void>
waiting_consumer(Channel<int>& ch, std::atomic<bool>& got_empty, std::atomic<bool>& done) {
    auto v = co_await ch.recv();
    if (!v) got_empty = true;
    done = true;
}

template<typename T>
Task<void> send_value(Channel<T>& ch, T value) {
    co_await ch.send(std::move(value));
}

template<typename T>
Task<std::optional<T>> receive_one(Channel<T>& ch) {
    co_return co_await ch.recv();
}

// Delay the caller's await_resume, not Channel internals: this models a
// selected receiver being preempted immediately after it is resumed.
template<typename Awaiter>
struct DelayedResume {
    Awaiter inner;
    std::binary_semaphore& resumed;
    std::binary_semaphore& proceed;

    bool await_ready() { return inner.await_ready(); }
    bool await_suspend(std::coroutine_handle<> h) { return inner.await_suspend(h); }
    auto await_resume() {
        resumed.release();
        proceed.acquire();
        return inner.await_resume();
    }
};

Task<std::optional<int>>
delayed_receiver(Channel<int>& ch, std::binary_semaphore& resumed, std::binary_semaphore& proceed) {
    // Explicit template argument: aggregate CTAD (P1816) is not available on
    // the CI runners' AppleClang, only on newer local toolchains.
    co_return co_await DelayedResume<decltype(ch.recv())>{ch.recv(), resumed, proceed};
}

Task<void>
concurrent_producer(Channel<int>& ch, int first, int count, std::atomic<int>& remaining) {
    for (int i = 0; i < count; ++i)
        co_await ch.send(first + i);
    if (remaining.fetch_sub(1) == 1) ch.close();
}

template<std::size_t N>
Task<void> concurrent_consumer(Channel<int>& ch,
                               std::array<std::atomic<int>, N>& received,
                               std::atomic<int>& remaining,
                               std::atomic<bool>& premature_eof) {
    while (auto value = co_await ch.recv()) {
        if (*value >= 0 && static_cast<std::size_t>(*value) < N) {
            received[static_cast<std::size_t>(*value)].fetch_add(1);
        }
    }
    if (!ch.is_closed()) premature_eof = true;
    remaining.fetch_sub(1);
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

TEST_CASE("Channel: selected receiver owns its delivery before await_resume") {
    Channel<int> ch{1};
    std::binary_semaphore resumed{0};
    std::binary_semaphore proceed{0};
    auto first = delayed_receiver(ch, resumed, proceed);
    first.start();
    CHECK_FALSE(first.done());

    std::thread sender([&] { send_value(ch, 42).blocking_get(); });
    const bool receiver_resumed = resumed.try_acquire_for(std::chrono::seconds{2});
    if (!receiver_resumed) {
        // Unblock either an already running or a still parked receiver before
        // joining and reporting failure, so this regression cannot hang CI.
        proceed.release();
        ch.close();
        sender.join();
        REQUIRE(receiver_resumed);
        return;
    }

    auto second = receive_one(ch);
    second.start();
    CHECK_FALSE(second.done());
    CHECK_FALSE(ch.is_closed());
    CHECK(ch.size() == 0);

    std::optional<int> expected_second;
    SUBCASE("another send goes to the second receiver") {
        send_value(ch, 43).blocking_get();
        expected_second = 43;
    }
    SUBCASE("close preserves the first delivery and wakes the second with EOF") {
        ch.close();
    }

    proceed.release();
    sender.join();
    const bool second_finished = second.done();
    ch.close();  // Release any waiter even if the implementation regresses.
    REQUIRE(first.done());
    REQUIRE(second_finished);
    CHECK(first.blocking_get() == std::optional<int>{42});
    CHECK(second.blocking_get() == expected_second);
}

TEST_CASE("Channel: capacity zero rendezvous in either arrival order") {
    Channel<int> ch{0};
    auto sender = send_value(ch, 17);
    auto receiver = receive_one(ch);

    SUBCASE("receiver arrives first") {
        receiver.start();
        CHECK_FALSE(receiver.done());
        sender.start();
    }
    SUBCASE("sender arrives first") {
        sender.start();
        CHECK_FALSE(sender.done());
        receiver.start();
    }

    const bool sender_finished = sender.done();
    const bool receiver_finished = receiver.done();
    CHECK(ch.size() == 0);
    CHECK_FALSE(ch.is_closed());
    ch.close();
    REQUIRE(sender_finished);
    REQUIRE(receiver_finished);
    CHECK(receiver.blocking_get() == std::optional<int>{17});
}

TEST_CASE("Channel: close releases all senders and drains only accepted values") {
    Channel<int> ch{1};
    send_value(ch, 1).blocking_get();
    auto second = send_value(ch, 2);
    auto third = send_value(ch, 3);
    second.start();
    third.start();
    CHECK_FALSE(second.done());
    CHECK_FALSE(third.done());

    ch.close();
    CHECK(second.done());
    CHECK(third.done());
    CHECK(ch.size() == 1);
    CHECK(receive_one(ch).blocking_get() == std::optional<int>{1});
    CHECK_FALSE(receive_one(ch).blocking_get().has_value());
    send_value(ch, 4).blocking_get();
    CHECK(ch.size() == 0);
    ch.close();
}

TEST_CASE("Channel: close releases every unassigned receiver") {
    Channel<int> ch{2};
    std::vector<Task<std::optional<int>>> receivers;
    for (int i = 0; i < 3; ++i) {
        receivers.push_back(receive_one(ch));
        receivers.back().start();
        CHECK_FALSE(receivers.back().done());
    }
    ch.close();
    for (auto& receiver : receivers) {
        REQUIRE(receiver.done());
        CHECK_FALSE(receiver.blocking_get().has_value());
    }
}

TEST_CASE("Channel: destruction completes parked rendezvous operations") {
    auto ch = std::make_unique<Channel<int>>(0);
    SUBCASE("parked receiver completes with EOF") {
        auto receiver = receive_one(*ch);
        receiver.start();
        CHECK_FALSE(receiver.done());
        ch.reset();
        REQUIRE(receiver.done());
        CHECK_FALSE(receiver.blocking_get().has_value());
    }
    SUBCASE("parked sender completes with its value dropped") {
        auto sender = send_value(*ch, 42);
        sender.start();
        CHECK_FALSE(sender.done());
        ch.reset();
        CHECK(sender.done());
    }
}

TEST_CASE("Channel: move-only values survive direct and buffered delivery") {
    Channel<std::unique_ptr<int>> ch{1};
    auto first = receive_one(ch);
    first.start();
    send_value(ch, std::make_unique<int>(10)).blocking_get();
    REQUIRE(first.done());
    auto first_value = first.blocking_get();
    REQUIRE(first_value.has_value());
    REQUIRE(*first_value != nullptr);
    CHECK(**first_value == 10);

    send_value(ch, std::make_unique<int>(20)).blocking_get();
    auto pending = send_value(ch, std::make_unique<int>(30));
    pending.start();
    CHECK_FALSE(pending.done());
    auto buffered = receive_one(ch).blocking_get();
    CHECK(pending.done());
    auto transferred = receive_one(ch).blocking_get();
    REQUIRE(buffered.has_value());
    REQUIRE(transferred.has_value());
    REQUIRE(*buffered != nullptr);
    REQUIRE(*transferred != nullptr);
    CHECK(**buffered == 20);
    CHECK(**transferred == 30);
}

TEST_CASE("Channel: bounded concurrent producers and consumers deliver exactly once") {
    constexpr int producer_count = 3;
    constexpr int consumer_count = 3;
    constexpr int values_per_producer = 64;
    Channel<int> ch{2};
    std::array<std::atomic<int>, producer_count * values_per_producer> received{};
    std::atomic<int> producers_remaining{producer_count};
    std::atomic<int> consumers_remaining{consumer_count};
    std::atomic<bool> premature_eof{false};
    std::barrier start_line{producer_count + consumer_count};
    std::vector<std::thread> workers;

    for (int i = 0; i < consumer_count; ++i) {
        workers.emplace_back([&] {
            start_line.arrive_and_wait();
            std::move(concurrent_consumer(ch, received, consumers_remaining, premature_eof))
                .start_detached();
        });
    }
    for (int i = 0; i < producer_count; ++i) {
        workers.emplace_back([&, i] {
            start_line.arrive_and_wait();
            std::move(concurrent_producer(
                          ch, i * values_per_producer, values_per_producer, producers_remaining))
                .start_detached();
        });
    }
    for (auto& worker : workers)
        worker.join();

    CHECK(producers_remaining.load() == 0);
    CHECK(consumers_remaining.load() == 0);
    CHECK_FALSE(premature_eof.load());
    for (const auto& count : received)
        CHECK(count.load() == 1);
    ch.close();  // Also releases any parked frames on a failed regression.
}
