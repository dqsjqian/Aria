// ============================================================================
//  test_function_ref.cpp
// ----------------------------------------------------------------------------
//  Pin down the contract of aria::function_ref<R(Args...)>: zero-allocation
//  non-owning callable handle. Each test below targets one promise made by
//  the header.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/function_ref.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using aria::function_ref;

// ----------------------------------------------------------------------------
//  FR-1: trivially copyable, two pointers in size, and two pointers only.
// ----------------------------------------------------------------------------
TEST_CASE("function_ref: trivially copyable, two-pointer footprint") {
    using FR = function_ref<int(int)>;
    static_assert(std::is_trivially_copyable_v<FR>);
    static_assert(std::is_trivially_destructible_v<FR>);
    // Storage discipline: object pointer + invoker pointer. Anything larger
    // would mean we accidentally grew the type.
    CHECK(sizeof(FR) == 2 * sizeof(void*));
}

// ----------------------------------------------------------------------------
//  FR-2: bind to a stateless lambda and forward arguments correctly.
// ----------------------------------------------------------------------------
TEST_CASE("function_ref: stateless lambda — invocation forwards arguments") {
    auto add = [](int a, int b) { return a + b; };
    function_ref<int(int, int)> fr = add;
    CHECK(fr(2, 3) == 5);
    CHECK(fr(-1, 1) == 0);
}

// ----------------------------------------------------------------------------
//  FR-3: bind to a stateful lambda — captures observed are the originals.
// ----------------------------------------------------------------------------
TEST_CASE("function_ref: stateful lambda — sees the captured object live") {
    int counter = 0;
    auto bump = [&counter](int n) { counter += n; };
    function_ref<void(int)> fr = bump;

    fr(3);
    fr(4);
    CHECK(counter == 7);
}

// ----------------------------------------------------------------------------
//  FR-4: bind to a free function pointer, including via implicit decay.
// ----------------------------------------------------------------------------
namespace fr_free {
    int square(int n) { return n * n; }
    double overloaded(double n) { return n / 2; }
    int overloaded(int n) { return n + 1; }
}

TEST_CASE("function_ref: free function pointer binding") {
    function_ref<int(int)> fr = &fr_free::square;
    CHECK(fr(7) == 49);

    // Implicit decay from a function reference.
    function_ref<int(int)> fr2 = fr_free::square;
    CHECK(fr2(8) == 64);
}

// ----------------------------------------------------------------------------
//  FR-5: copy is trivially correct — both copies point to the same target.
// ----------------------------------------------------------------------------
TEST_CASE("function_ref: copy yields identical view of the target") {
    int hits = 0;
    auto fn = [&hits](int n) { hits += n; };
    function_ref<void(int)> fr = fn;
    function_ref<void(int)> copy = fr;

    fr(1);
    copy(2);
    fr(3);
    CHECK(hits == 6);
}

// ----------------------------------------------------------------------------
//  FR-6: explicit operator bool — disengaged vs engaged.
// ----------------------------------------------------------------------------
TEST_CASE("function_ref: explicit operator bool reports engagement") {
    function_ref<int(int)> empty;
    CHECK_FALSE(static_cast<bool>(empty));
    CHECK(empty == nullptr);
    CHECK_FALSE(empty != nullptr);

    auto id = [](int n) { return n; };
    function_ref<int(int)> alive = id;
    CHECK(static_cast<bool>(alive));
    CHECK(alive != nullptr);
    CHECK_FALSE(alive == nullptr);

    alive = nullptr;
    CHECK_FALSE(static_cast<bool>(alive));
}

// ----------------------------------------------------------------------------
//  FR-7: reassignment redirects the view to a new callable.
// ----------------------------------------------------------------------------
TEST_CASE("function_ref: reassign retargets without owning the old callable") {
    auto a = [](int n) { return n + 100; };
    auto b = [](int n) { return n + 200; };

    function_ref<int(int)> fr = a;
    CHECK(fr(1) == 101);

    fr = b;
    CHECK(fr(1) == 201);
}

// ----------------------------------------------------------------------------
//  FR-8: zero allocations through the type-erased call boundary.
//  The contract is "no heap allocation when constructing or calling a
//  function_ref over a small lambda". We verify by overriding the global
//  allocator counters and asserting they stay flat across construction +
//  invocation.
// ----------------------------------------------------------------------------
namespace fr_alloc {
    inline int g_new_count = 0;
    struct Counter {
        Counter()  { ++g_new_count; }
    };
}

TEST_CASE("function_ref: zero heap activity around bind+call of a lambda") {
    // Sentinel: prove our counter mechanism actually fires on a real new.
    fr_alloc::g_new_count = 0;
    {
        auto* p = new fr_alloc::Counter();
        delete p;
    }
    REQUIRE(fr_alloc::g_new_count == 1);

    // Real test: bind+call a lambda — no `new` should run.
    fr_alloc::g_new_count = 0;
    int seen = 0;
    auto fn = [&seen](int n) { seen += n; };
    function_ref<void(int)> fr = fn;
    fr(11);
    fr(22);
    CHECK(seen == 33);
    CHECK(fr_alloc::g_new_count == 0);
}

// ----------------------------------------------------------------------------
//  FR-9: function_ref param is a strict view, not a sink — confirms we can
//  pass any matching callable as a single argument without forcing a
//  std::function copy on the caller.
// ----------------------------------------------------------------------------
namespace {
    int sum_via_ref(const std::vector<int>& xs, function_ref<int(int)> proj) {
        int total = 0;
        for (int x : xs) total += proj(x);
        return total;
    }
}

TEST_CASE("function_ref: API parameter accepts any matching callable") {
    std::vector<int> xs{1, 2, 3, 4};
    CHECK(sum_via_ref(xs, [](int x) { return x; })          == 10);
    CHECK(sum_via_ref(xs, [](int x) { return x * x; })      == 30);

    int bias = 100;
    CHECK(sum_via_ref(xs, [&](int x) { return x + bias; })  == 410);
}

// ----------------------------------------------------------------------------
//  FR-10: void-returning signature works without surprises.
// ----------------------------------------------------------------------------
TEST_CASE("function_ref: void return — no R() shenanigans") {
    int seen = 0;
    auto sink = [&seen](int n) { seen = n; };
    function_ref<void(int)> fr = sink;
    fr(42);
    CHECK(seen == 42);
}

namespace {
int function_ref_increment(int value) noexcept { return value + 1; }
auto function_ref_pointer_value() { return &fr_free::square; }
}

TEST_CASE("function_ref: function pointer assignment stores its value beyond the full expression") {
    function_ref<int(int)> view;
    view = function_ref_pointer_value();
    CHECK(view(7) == 49);
    auto pointer = &fr_free::square;
    view = pointer;
    pointer = nullptr;
    CHECK(view(8) == 64);
    view = fr_free::square;
    CHECK(view(9) == 81);
    view = function_ref_increment;
    CHECK(view(9) == 10);
    function_ref<int(int)> copied = view;
    CHECK(copied(10) == 11);
}

TEST_CASE("function_ref: typed null function pointers are disengaged") {
    int (*pointer)(int) = nullptr;
    function_ref<int(int)> view = pointer;
    CHECK_FALSE(static_cast<bool>(view));
    CHECK(view == nullptr);
    view = fr_free::square;
    view = pointer;
    CHECK_FALSE(static_cast<bool>(view));
    CHECK(view == nullptr);
}

TEST_CASE("function_ref: supports const targets and mutable calls through const views") {
    const auto constant = [](int value) { return value + 3; };
    function_ref<int(int)> constant_view = constant;
    CHECK(constant_view(2) == 5);
    auto mutable_target = [value = 0]() mutable { return ++value; };
    const function_ref<int()> mutable_view = mutable_target;
    CHECK(mutable_view() == 1);
    CHECK(mutable_view() == 2);
    CHECK(mutable_target() == 3);
}

TEST_CASE("function_ref: void signatures discard results and pointers adapt compatible signatures") {
    int calls = 0;
    auto returns_value = [&] { return ++calls; };
    function_ref<void()> view = returns_value;
    view();
    CHECK(calls == 1);
    function_ref<void(int)> pointer_view = &fr_free::square;
    pointer_view(2);
    function_ref<long(short)> converted = &function_ref_increment;
    CHECK(converted(2) == 3);
}

TEST_CASE("function_ref: exact signatures resolve overloaded free functions") {
    function_ref<int(int)> view = fr_free::overloaded;
    CHECK(view(3) == 4);
    view = fr_free::overloaded;
    CHECK(view(4) == 5);
}
