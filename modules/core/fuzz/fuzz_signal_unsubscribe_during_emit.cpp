// L-13: canceling a pending slot skips it in the current fan-out; a slot
// registered during fan-out starts at the next emission. The controller is
// registered first so random cancellations occur before user callbacks.
#include <doctest/doctest.h>
#include "aria/detail/typed_signal.hpp"
#include "fuzz_support.hpp"
#include <vector>

using namespace aria;

TEST_CASE("L-13 fuzz: cancellation and registration during fan-out") {
    aria::detail::TypedSignal<int> sig;
    fuzz::Rng rng{fuzz::seed(0xA1A1'1313)};
    std::vector<Subscription> subs;
    std::size_t calls = 0;
    bool removed = false;
    auto make_handler = [&] {
        return [&](const int&) noexcept { ++calls; };
    };
    auto controller = sig.connect([&](const int&) {
        removed = false;
        if (!subs.empty() && rng.coin(0.5)) {
            const auto i = rng.u32(0, static_cast<std::uint32_t>(subs.size() - 1));
            subs.erase(subs.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
        }
        if (rng.coin(0.5)) subs.push_back(sig.connect(make_handler()));
    });
    for (int i = 0; i < 4; ++i) subs.push_back(sig.connect(make_handler()));
    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const auto live = subs.size();
        const auto before = calls;
        sig.emit(static_cast<int>(step));
        CHECK(calls - before == live - (removed ? 1U : 0U));
        CHECK(sig.slot_count() == subs.size() + 1);
        if (subs.size() > 32) subs.resize(32);
    }
}
