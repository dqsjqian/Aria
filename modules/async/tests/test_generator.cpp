#include <doctest/doctest.h>

#include "aria/async/generator.hpp"

#include <atomic>
#include <stdexcept>
#include <vector>

using namespace aria::async;

namespace {

Generator<int> count_up_to(int n) {
    for (int i = 0; i < n; ++i) co_yield i;
}

Generator<int> empty_gen() { co_return; }

Generator<int> throws_at(int idx) {
    for (int i = 0;; ++i) {
        if (i == idx) throw std::runtime_error("yielded into a wall");
        co_yield i;
    }
}

}  // namespace

TEST_CASE("Generator: produces all values via range-for") {
    std::vector<int> out;
    for (int v : count_up_to(5)) out.push_back(v);
    CHECK(out == std::vector<int>{0, 1, 2, 3, 4});
}

TEST_CASE("Generator: empty generator iterates zero times") {
    int n = 0;
    for (int v : empty_gen()) { (void)v; ++n; }
    CHECK(n == 0);
}

TEST_CASE("Generator: exception inside coroutine surfaces during iteration") {
    auto gen = throws_at(2);
    auto it = gen.begin();
    CHECK(*it == 0);
    ++it;
    CHECK(*it == 1);
    CHECK_THROWS_AS(++it, std::runtime_error);
}

// NOTE: cannot use lambda captures-by-reference for the body of a Generator
// (or any coroutine). The lambda object goes out of scope at the call site,
// leaving dangling references inside the coroutine frame. Use a free function
// or pass shared state via parameters.
namespace {
Generator<int> count_with_counter(std::atomic<int>& produced, int n) {
    for (int i = 0; i < n; ++i) {
        produced.fetch_add(1);
        co_yield i;
    }
}
}

TEST_CASE("Generator: lazy — values produced on demand") {
    std::atomic<int> produced{0};
    auto g = count_with_counter(produced, 3);
    auto&& it = g.begin();
    CHECK(produced.load() == 1);
    ++it;
    CHECK(produced.load() == 2);
    ++it;
    CHECK(produced.load() == 3);
}

namespace {
struct YieldMoveFailure {
    std::atomic<bool>* fail = nullptr;
    YieldMoveFailure() = default;
    explicit YieldMoveFailure(std::atomic<bool>& fail_assignment) : fail(&fail_assignment) {}
    YieldMoveFailure(YieldMoveFailure&&) = default;
    YieldMoveFailure& operator=(YieldMoveFailure&& other) {
        if (other.fail != nullptr && other.fail->load()) {
            throw std::runtime_error("yield move assignment");
        }
        fail = other.fail;
        return *this;
    }
};
// The test owns the flag until both generator instances have been destroyed.
Generator<YieldMoveFailure> yield_move_failure(std::atomic<bool>& fail) {
    co_yield YieldMoveFailure{fail};
}
}

TEST_CASE("Generator: throwing yield storage propagates to its consumer") {
    std::atomic<bool> fail{true};
    auto generator = yield_move_failure(fail);
    CHECK_THROWS_WITH_AS(generator.begin(), "yield move assignment", std::runtime_error);

    fail.store(false);
    auto succeeding_generator = yield_move_failure(fail);
    auto iterator = succeeding_generator.begin();
    REQUIRE(iterator != std::default_sentinel);
    CHECK(iterator->fail == &fail);
    ++iterator;
    CHECK(iterator == std::default_sentinel);
}
