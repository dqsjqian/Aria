#include <doctest/doctest.h>

#include "aria/runtime/event_bus.hpp"
#include <string>

using namespace aria::runtime;
using namespace aria;

namespace {
struct PingEvent { int n; };
struct NamedEvent { std::string name; };
}  // namespace

TEST_CASE("EventBus: publish reaches subscribers") {
    EventBus bus;
    int received = 0;
    auto sub = bus.subscribe<PingEvent>([&](const PingEvent& e) { received = e.n; });
    bus.publish(PingEvent{42});
    CHECK(received == 42);
}

TEST_CASE("EventBus: multiple subscribers all fire") {
    EventBus bus;
    int a = 0, b = 0;
    auto s1 = bus.subscribe<PingEvent>([&](const PingEvent& e) { a = e.n; });
    auto s2 = bus.subscribe<PingEvent>([&](const PingEvent& e) { b = e.n; });
    bus.publish(PingEvent{7});
    CHECK(a == 7);
    CHECK(b == 7);
}

TEST_CASE("EventBus: unsubscribe stops delivery") {
    EventBus bus;
    int n = 0;
    {
        auto sub = bus.subscribe<PingEvent>([&](const PingEvent& e) { n = e.n; });
        bus.publish(PingEvent{1});
        CHECK(n == 1);
    }
    bus.publish(PingEvent{99});
    CHECK(n == 1);
}

TEST_CASE("EventBus: types are kept separate") {
    EventBus bus;
    int p = 0;
    std::string s;
    auto sa = bus.subscribe<PingEvent>([&](const PingEvent& e) { p = e.n; });
    auto sb = bus.subscribe<NamedEvent>([&](const NamedEvent& e) { s = e.name; });
    bus.publish(PingEvent{5});
    bus.publish(NamedEvent{"hello"});
    CHECK(p == 5);
    CHECK(s == "hello");
}

TEST_CASE("EventBus: global() returns same instance") {
    auto& a = EventBus::global();
    auto& b = EventBus::global();
    CHECK(&a == &b);
}
