#include <doctest/doctest.h>

#include "aria/async/task.hpp"
#include <stdexcept>
#include <string>

using namespace aria::async;

namespace {

Task<int> make_int(int x) { co_return x * 2; }
Task<void> make_void() { co_return; }

Task<std::string> make_string() {
    auto a = co_await make_int(7);
    co_return std::string("got ") + std::to_string(a);
}

Task<int> throws_one() {
    throw std::runtime_error("boom");
    co_return 0;
}

}  // namespace

TEST_CASE("Task<int>: blocking_get returns value") {
    auto t = make_int(21);
    CHECK(t.blocking_get() == 42);
}

TEST_CASE("Task<void>: blocking_get does not throw") {
    auto t = make_void();
    t.blocking_get();
    CHECK(t.done());
}

TEST_CASE("Task: chained co_await works") {
    auto t = make_string();
    CHECK(t.blocking_get() == "got 14");
}

TEST_CASE("Task: exceptions propagate via blocking_get") {
    auto t = throws_one();
    CHECK_THROWS_AS(t.blocking_get(), std::runtime_error);
}
