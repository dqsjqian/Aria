// ============================================================================
//  test_numeric_saturate.cpp
// ----------------------------------------------------------------------------
//  Contract tests for binding/detail/numeric_saturate.hpp:
//    * NS-1  saturate clamps int64 over INT_MAX
//    * NS-2  saturate clamps int64 under INT_MIN
//    * NS-3  saturate clamps uint64 over INT_MAX
//    * NS-4  in-range values pass through untouched
//    * NS-5  int_to_uint64_clamped flips negatives to 0
//    * NS-6  warn fires exactly once per (label, direction) per process
//    * NS-7  different directions on the same label are independent budgets
//    * NS-8  different labels have independent budgets
//    * NS-9  reset_saturate_warning_dedup_for_testing() lets the contract
//            be re-observed in subsequent test cases
// ============================================================================

#include <doctest/doctest.h>

#include "aria/binding/detail/numeric_saturate.hpp"
#include "aria/runtime/logger.hpp"

#include <climits>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct CapturedWarn {
    aria::runtime::LogLevel level;
    std::string             category;
    std::string             message;
};

class WarnCaptureScope {
public:
    WarnCaptureScope() {
        ::aria::binding::detail::reset_saturate_warning_dedup_for_testing();
        log_.clear();
        ::aria::runtime::Logger::instance().set_sink(
            [this](aria::runtime::LogLevel lvl,
                   std::string_view        cat,
                   std::string_view        msg) {
                std::lock_guard lk(mu_);
                log_.push_back(CapturedWarn{
                    lvl, std::string{cat}, std::string{msg}});
            });
    }
    ~WarnCaptureScope() {
        ::aria::runtime::Logger::instance().set_sink(nullptr);
        ::aria::binding::detail::reset_saturate_warning_dedup_for_testing();
    }

    [[nodiscard]] std::size_t warn_count_for(std::string_view category) const {
        std::lock_guard lk(mu_);
        std::size_t n = 0;
        for (auto const& e : log_) {
            if (e.level == aria::runtime::LogLevel::Warn && e.category == category) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] std::size_t total_warn_count() const {
        std::lock_guard lk(mu_);
        std::size_t n = 0;
        for (auto const& e : log_) {
            if (e.level == aria::runtime::LogLevel::Warn) ++n;
        }
        return n;
    }

private:
    mutable std::mutex       mu_;
    std::vector<CapturedWarn> log_;
};

}  // namespace

// ----------------------------------------------------------------------------
//  NS-1: int64 over INT_MAX clamps to INT_MAX.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-1 int64 over INT_MAX clamps") {
    WarnCaptureScope scope;
    const auto huge = static_cast<std::int64_t>(INT_MAX) + 7;
    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, "test::ns1") == INT_MAX);
}

// ----------------------------------------------------------------------------
//  NS-2: int64 under INT_MIN clamps to INT_MIN.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-2 int64 under INT_MIN clamps") {
    WarnCaptureScope scope;
    const auto tiny = static_cast<std::int64_t>(INT_MIN) - 7;
    CHECK(::aria::binding::detail::saturate_int64_to_int(tiny, "test::ns2") == INT_MIN);
}

// ----------------------------------------------------------------------------
//  NS-3: uint64 over INT_MAX clamps to INT_MAX.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-3 uint64 over INT_MAX clamps") {
    WarnCaptureScope scope;
    const auto huge = static_cast<std::uint64_t>(INT_MAX) + 7ULL;
    CHECK(::aria::binding::detail::saturate_uint64_to_int(huge, "test::ns3") == INT_MAX);
}

// ----------------------------------------------------------------------------
//  NS-4: in-range values pass through with no warning.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-4 in-range pass-through is silent") {
    WarnCaptureScope scope;
    CHECK(::aria::binding::detail::saturate_int64_to_int(42, "test::ns4-i64") == 42);
    CHECK(::aria::binding::detail::saturate_uint64_to_int(42ULL, "test::ns4-u64") == 42);
    CHECK(::aria::binding::detail::saturate_int64_to_int(0, "test::ns4-i64") == 0);
    CHECK(::aria::binding::detail::saturate_int64_to_int(-1, "test::ns4-i64") == -1);
    CHECK(scope.total_warn_count() == 0);
}

// ----------------------------------------------------------------------------
//  NS-5: int_to_uint64_clamped sends negatives to 0.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-5 int_to_uint64_clamped flips negatives to 0") {
    CHECK(::aria::binding::detail::int_to_uint64_clamped(-1) == 0ULL);
    CHECK(::aria::binding::detail::int_to_uint64_clamped(0) == 0ULL);
    CHECK(::aria::binding::detail::int_to_uint64_clamped(7) == 7ULL);
    CHECK(::aria::binding::detail::int_to_uint64_clamped(INT_MAX)
          == static_cast<std::uint64_t>(INT_MAX));
}

// ----------------------------------------------------------------------------
//  NS-6: warn fires EXACTLY ONCE per (label, direction).
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-6 warn-once-per-label-direction") {
    WarnCaptureScope scope;
    constexpr std::string_view label = "test::ns6";
    const auto huge = static_cast<std::int64_t>(INT_MAX) + 1;

    // First overflow on this label: warn fires.
    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, label) == INT_MAX);
    CHECK(scope.warn_count_for(label) == 1);

    // Subsequent overflows on the same (label, direction): silent.
    for (int i = 0; i < 100; ++i) {
        CHECK(::aria::binding::detail::saturate_int64_to_int(huge, label) == INT_MAX);
    }
    CHECK(scope.warn_count_for(label) == 1);
}

// ----------------------------------------------------------------------------
//  NS-7: different directions on the same label have independent budgets.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-7 over and under share label, separate budgets") {
    WarnCaptureScope scope;
    constexpr std::string_view label = "test::ns7";
    const auto huge = static_cast<std::int64_t>(INT_MAX) + 1;
    const auto tiny = static_cast<std::int64_t>(INT_MIN) - 1;

    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, label) == INT_MAX);
    CHECK(scope.warn_count_for(label) == 1);

    // First UNDERflow on the same label: a *fresh* warn fires (different direction).
    CHECK(::aria::binding::detail::saturate_int64_to_int(tiny, label) == INT_MIN);
    CHECK(scope.warn_count_for(label) == 2);

    // Second UNDERflow: silent.
    CHECK(::aria::binding::detail::saturate_int64_to_int(tiny, label) == INT_MIN);
    CHECK(scope.warn_count_for(label) == 2);
}

// ----------------------------------------------------------------------------
//  NS-8: different labels are completely independent.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-8 different labels independent budgets") {
    WarnCaptureScope scope;
    const auto huge = static_cast<std::int64_t>(INT_MAX) + 1;

    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, "test::ns8-A") == INT_MAX);
    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, "test::ns8-B") == INT_MAX);
    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, "test::ns8-A") == INT_MAX);  // dup
    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, "test::ns8-B") == INT_MAX);  // dup

    CHECK(scope.warn_count_for("test::ns8-A") == 1);
    CHECK(scope.warn_count_for("test::ns8-B") == 1);
}

// ----------------------------------------------------------------------------
//  NS-9: reset_saturate_warning_dedup_for_testing() lets us see the
//  first-warn path again in a fresh test.
// ----------------------------------------------------------------------------
TEST_CASE("numeric_saturate: NS-9 reset_for_testing re-arms the budget") {
    WarnCaptureScope scope;
    constexpr std::string_view label = "test::ns9";
    const auto huge = static_cast<std::int64_t>(INT_MAX) + 1;

    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, label) == INT_MAX);
    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, label) == INT_MAX);
    CHECK(scope.warn_count_for(label) == 1);

    // Reset and re-trigger: the next overflow warns again.
    ::aria::binding::detail::reset_saturate_warning_dedup_for_testing();
    CHECK(::aria::binding::detail::saturate_int64_to_int(huge, label) == INT_MAX);
    CHECK(scope.warn_count_for(label) == 2);
}
