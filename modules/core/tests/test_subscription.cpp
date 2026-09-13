#include <doctest/doctest.h>

#include "aria/subscription.hpp"
#include "aria/callback_boundary.hpp"

#include <stdexcept>
#include <vector>

using namespace aria;

TEST_CASE("SubscriptionBag: clear and destruction release in reverse order") {
    std::vector<int> order;
    {
        SubscriptionBag bag;
        bag += Subscription{[&] { order.push_back(1); }};
        bag += Subscription{[&] { order.push_back(2); }};
        bag.clear();
        CHECK(order == std::vector<int>{2, 1});
        bag += Subscription{[&] { order.push_back(3); }};
        bag += Subscription{[&] { order.push_back(4); }};
    }
    CHECK(order == std::vector<int>{2, 1, 4, 3});
}

TEST_CASE("SubscriptionBag: disconnect can clear and add a fresh connection") {
    SubscriptionBag bag;
    std::vector<int> order;
    bag += Subscription{[&] { order.push_back(1); }};
    bag += Subscription{[&] {
        order.push_back(2);
        bag.clear();
        bag += Subscription{[&] { order.push_back(3); }};
    }};
    bag.clear();
    CHECK(order == std::vector<int>{2, 1});
    CHECK(bag.size() == 1);
    bag.clear();
    CHECK(order == std::vector<int>{2, 1, 3});
}

TEST_CASE("SubscriptionBag: move assignment releases old subscriptions in reverse order") {
    std::vector<int> order;
    SubscriptionBag destination;
    destination += Subscription{[&] { order.push_back(1); }};
    destination += Subscription{[&] { order.push_back(2); }};
    SubscriptionBag source;
    source += Subscription{[&] { order.push_back(3); }};
    destination = std::move(source);
    CHECK(order == std::vector<int>{2, 1});
    CHECK(source.empty());
    destination.clear();
    CHECK(order == std::vector<int>{2, 1, 3});
}

namespace {
int disconnect_failures = 0;
void on_disconnect_failure(const CallbackFailure& failure) {
    if (failure.category == "subscription.disconnect") ++disconnect_failures;
}
}

TEST_CASE("Subscription: throwing disconnect reports failure and finishes teardown") {
    disconnect_failures = 0;
    const auto previous = set_callback_failure_sink(&on_disconnect_failure);
    Subscription subscription{[] { throw std::runtime_error("disconnect failed"); }};
    subscription.release();
    CHECK_FALSE(subscription.active());
    CHECK(disconnect_failures == 1);
    set_callback_failure_sink(previous);
}

namespace {
int recursive_failure_calls = 0;
void recursive_failure_sink(const aria::CallbackFailure&) {
    ++recursive_failure_calls;
    aria::report_callback_failure("test.recursive", "nested report");
}
}

TEST_CASE("Callback boundary: recursive reporting falls back without invoking the sink again") {
    recursive_failure_calls = 0;
    auto previous = aria::set_callback_failure_sink(recursive_failure_sink);
    aria::report_callback_failure("test.outer", "outer report");
    aria::set_callback_failure_sink(previous);
    CHECK(recursive_failure_calls == 1);
}

TEST_CASE("Subscription: move-only captures are stored and disconnected once") {
    int calls = 0;
    aria::Subscription subscription{[value = std::make_unique<int>(7), &calls] {
        CHECK(*value == 7);
        ++calls;
    }};
    aria::Subscription moved{std::move(subscription)};
    CHECK_FALSE(subscription);
    moved.release();
    CHECK(calls == 1);
}

TEST_CASE("Subscription: empty function and function pointer are inactive") {
    aria::Subscription empty_function{std::function<void()>{}};
    void (*empty_pointer)() = nullptr;
    aria::Subscription empty_callback{empty_pointer};
    CHECK_FALSE(empty_function);
    CHECK_FALSE(empty_callback);
}
