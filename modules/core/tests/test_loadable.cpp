// ============================================================================
//  test_loadable.cpp
// ----------------------------------------------------------------------------
//  Pin down `Loadable<T>` per the LO-N invariants laid out in
//  modules/core/include/aria/loadable.hpp. Each TEST_CASE references
//  the canonical invariant ID so a failure points the reader straight
//  at the authoritative description.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/aria.hpp"

#include <string>

using namespace aria;

// ----------------------------------------------------------------------------
//  LO-1: five-state discriminator
// ----------------------------------------------------------------------------
TEST_CASE("LO-1: idle is the default-constructed state") {
    Loadable<int> l;
    CHECK(l.state() == LoadState::Idle);
    CHECK(l.is_idle());
    CHECK_FALSE(l.in_flight());
    CHECK_FALSE(l.has_value());
    CHECK_FALSE(l.has_error());
    CHECK(l.value() == nullptr);
    CHECK(l.error() == nullptr);
}

TEST_CASE("LO-1: loading() carries no value, no error") {
    auto l = Loadable<int>::loading();
    CHECK(l.is_loading());
    CHECK(l.in_flight());
    CHECK_FALSE(l.has_value());
    CHECK_FALSE(l.has_error());
}

TEST_CASE("LO-1: refreshing(prior) preserves the prior value (SWR)") {
    auto l = Loadable<int>::refreshing(42);
    CHECK(l.is_refreshing());
    CHECK(l.in_flight());
    REQUIRE(l.has_value());
    CHECK(*l.value() == 42);
    CHECK_FALSE(l.has_error());
}

TEST_CASE("LO-1: success(v) holds value, in_flight = false") {
    auto l = Loadable<std::string>::success("ok");
    CHECK(l.is_success());
    CHECK_FALSE(l.in_flight());
    REQUIRE(l.has_value());
    CHECK(*l.value() == "ok");
}

TEST_CASE("LO-1: error(err) without prior") {
    auto err = Error::async_failure("boom", "Test");
    auto l   = Loadable<int>::error(err);
    CHECK(l.is_error());
    CHECK_FALSE(l.in_flight());
    CHECK_FALSE(l.has_value());
    REQUIRE(l.has_error());
    CHECK(l.error()->message == "boom");
    CHECK(l.error()->kind    == ErrorKind::AsyncFailure);
}

TEST_CASE("LO-1: error(err, prior) keeps last-good value (SWR)") {
    auto err = Error::timeout("Test");
    auto l   = Loadable<int>::error(err, 7);
    CHECK(l.is_error());
    REQUIRE(l.has_value());
    CHECK(*l.value() == 7);
    REQUIRE(l.has_error());
    CHECK(l.error()->kind == ErrorKind::Timeout);
}

// ----------------------------------------------------------------------------
//  LO-3: in_flight() collapses Loading|Refreshing
// ----------------------------------------------------------------------------
TEST_CASE("LO-3: in_flight() iff Loading or Refreshing") {
    CHECK_FALSE(Loadable<int>::idle().in_flight());
    CHECK      (Loadable<int>::loading().in_flight());
    CHECK      (Loadable<int>::refreshing(1).in_flight());
    CHECK_FALSE(Loadable<int>::success(1).in_flight());
    CHECK_FALSE(Loadable<int>::error(Error::async_failure("x", "T"))
                   .in_flight());
}

// ----------------------------------------------------------------------------
//  LO-4: value_or fallback is non-throwing
// ----------------------------------------------------------------------------
TEST_CASE("LO-4: value_or returns the value when present") {
    CHECK(Loadable<int>::success(99).value_or(0) == 99);
}

TEST_CASE("LO-4: value_or returns the fallback when absent") {
    CHECK(Loadable<int>::idle().value_or(7)    == 7);
    CHECK(Loadable<int>::loading().value_or(7) == 7);
    auto err_no_prior = Loadable<int>::error(Error::async_failure("x", "T"));
    CHECK(err_no_prior.value_or(7) == 7);
}

// ----------------------------------------------------------------------------
//  LO-5: equality is structural (state + value + error)
// ----------------------------------------------------------------------------
TEST_CASE("LO-5: equality is structural across all five states") {
    CHECK(Loadable<int>::idle()          == Loadable<int>::idle());
    CHECK(Loadable<int>::loading()       == Loadable<int>::loading());
    CHECK(Loadable<int>::success(1)      == Loadable<int>::success(1));
    CHECK(Loadable<int>::success(1)      != Loadable<int>::success(2));
    CHECK(Loadable<int>::refreshing(1)   != Loadable<int>::loading());
    CHECK(Loadable<int>::refreshing(1)   != Loadable<int>::success(1));

    auto err = Error::async_failure("x", "T");
    CHECK(Loadable<int>::error(err)      == Loadable<int>::error(err));
    CHECK(Loadable<int>::error(err, 5)   != Loadable<int>::error(err));
}

// ----------------------------------------------------------------------------
//  LO-6: map projects the value, leaves state / error untouched
// ----------------------------------------------------------------------------
TEST_CASE("LO-6: map on success projects the value") {
    auto l   = Loadable<int>::success(21);
    auto out = l.map([](int x) { return x * 2; });
    CHECK(out.is_success());
    REQUIRE(out.has_value());
    CHECK(*out.value() == 42);
}

TEST_CASE("LO-6: map on idle / loading preserves state with no value") {
    auto out_idle = Loadable<int>::idle().map([](int x) { return x + 1; });
    CHECK(out_idle.is_idle());
    CHECK_FALSE(out_idle.has_value());

    auto out_loading = Loadable<int>::loading().map([](int x) { return x + 1; });
    CHECK(out_loading.is_loading());
    CHECK_FALSE(out_loading.has_value());
}

TEST_CASE("LO-6: map on refreshing projects the prior value") {
    auto out = Loadable<int>::refreshing(10).map([](int x) { return x + 5; });
    CHECK(out.is_refreshing());
    REQUIRE(out.has_value());
    CHECK(*out.value() == 15);
}

TEST_CASE("LO-6: map on error preserves state + error; projects prior if present") {
    auto err  = Error::validation(ValidationKey{"", ""}, "bad");
    auto e1   = Loadable<int>::error(err).map([](int x) { return x + 1; });
    CHECK(e1.is_error());
    CHECK_FALSE(e1.has_value());
    REQUIRE(e1.has_error());
    CHECK(e1.error()->message == "bad");

    auto e2 = Loadable<int>::error(err, 100).map([](int x) { return x * 10; });
    CHECK(e2.is_error());
    REQUIRE(e2.has_value());
    CHECK(*e2.value() == 1000);
    REQUIRE(e2.has_error());
    CHECK(e2.error()->message == "bad");
}

TEST_CASE("LO-6: map can change the value type") {
    auto out = Loadable<int>::success(42)
                   .map([](int x) { return std::to_string(x); });
    static_assert(std::is_same_v<decltype(out), Loadable<std::string>>);
    REQUIRE(out.has_value());
    CHECK(*out.value() == "42");
}
