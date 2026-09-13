#include <doctest/doctest.h>

#include "fuzz_support.hpp"

#include <limits>
#include <string>

TEST_CASE("Fuzz iterations: accept complete positive counts representable by size_t") {
    CHECK(aria::fuzz::parse_iters("1") == 1);
    CHECK(aria::fuzz::parse_iters("1000000") == 1'000'000);
    CHECK(aria::fuzz::parse_iters("0007") == 7);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    CHECK(aria::fuzz::parse_iters(std::to_string(maximum)) == maximum);
}

TEST_CASE("Fuzz iterations: invalid input falls back without disabling the suite") {
    for (const auto* text : {"", "0", "-1", "+1", " 7", "7 ", "12garbage", "1.5",
                             "18446744073709551616"}) {
        CAPTURE(text);
        CHECK(aria::fuzz::parse_iters(text) == aria::fuzz::kDefaultIters);
    }
    const auto overflow = std::to_string(std::numeric_limits<std::size_t>::max()) + "0";
    CHECK(aria::fuzz::parse_iters(overflow) == aria::fuzz::kDefaultIters);
    CHECK(aria::fuzz::kDefaultIters > 0);
}
