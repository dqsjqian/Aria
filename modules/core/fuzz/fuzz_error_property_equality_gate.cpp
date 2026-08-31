// ============================================================================
//  fuzz_error_property_equality_gate.cpp  (E-11 / E-10 / L-21)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "`inner` does NOT participate in equality. The Property
//     write-equality gate (L-21) compares kind / severity / message /
//     source / key. Consequence: writing the same logical error twice
//     does NOT re-notify observers."
//
//  This is the contract that keeps a retry loop from spamming the UI:
//  ten identical failures must produce one notification, not ten. The
//  subtlety is that each of those ten failures carries a DIFFERENT
//  `exception_ptr` (a fresh throw each time), so equality has to
//  deliberately ignore `inner` — and `exception_ptr` comparison is
//  pointer identity, so a naive `operator==` that included it would
//  make every write "different" and the gate would never fire.
//
//  Why fuzz rather than assert once: the gate is a *sequence* property.
//  A single "write twice, expect one notification" test passes even if
//  the gate is confused by, say, an equal-message-but-different-kind
//  pair, or by a transition through nullopt. Here the expected
//  notification count is tracked against a reference model over a long
//  random walk, so any disagreement between "Error equality" and
//  "observer fired" surfaces.
//
//  Strategy:
//    - Hold `Property<std::optional<Error>>`, mirroring the AsyncCommand
//      / AsyncResource error face (E-20 / E-21).
//    - Random walk: write a randomly-picked Error from a small pool,
//      write nullopt, or re-write the current value. Each pool entry is
//      rebuilt fresh on every write so `inner` differs every time.
//    - A reference model computes whether the value CHANGED by value;
//      the observer count must match it exactly.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/error.hpp"
#include "aria/property.hpp"
#include "aria/reactive/reactive.hpp"
#include "fuzz_support.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aria;

namespace {

/// Rebuild one of a few logically-distinct Errors. Called fresh on
/// every write so the `inner` exception_ptr is a new object each time
/// even when the logical error is identical.
Error make_error(int which) {
    switch (which) {
    case 0: {
        // Carries `inner`, so successive rebuilds differ in pointer
        // identity while remaining equal by value.
        std::exception_ptr ep;
        try {
            throw std::runtime_error("boom");
        } catch (...) {
            ep = std::current_exception();
        }
        return Error::from_exception(ep, "FuzzSurface");
    }
    case 1:
        return Error::timeout("FuzzSurface");
    case 2:
        return Error::user_error("bad input", "FuzzSurface");
    case 3:
        // Same message as case 2 but a different kind — pins that the
        // gate compares kind too, not just the message.
        return Error::async_failure("bad input", "FuzzSurface");
    default:
        return Error::validation(ValidationKey{"email", "rule_1"}, "invalid");
    }
}

constexpr int kPoolSize = 5;

/// Value-equality spelled out FIELD BY FIELD, deliberately not calling
/// `operator==(Error, Error)`.
///
/// This is the whole point of the reference model. Using the library's
/// own `operator==` here would make the assertion below a tautology:
/// mutate `operator==` to also compare `inner` and the model would
/// adopt the same mutation, so the expected count would track the
/// broken behaviour and the fuzzer would report success. Spelling out
/// E-11's field list independently means the model encodes the
/// *contract*, and any drift between the contract and the
/// implementation shows up as a mismatch.
///
/// E-11's field list: kind / severity / message / source / key.
/// `inner` is excluded — that exclusion is the invariant.
bool same_by_contract(const Error& a, const Error& b) {
    return a.kind     == b.kind
        && a.severity == b.severity
        && a.message  == b.message
        && a.source   == b.source
        && a.key.field_path == b.key.field_path
        && a.key.rule_id    == b.key.rule_id;
}

bool same_by_contract(const std::optional<Error>& a,
                      const std::optional<Error>& b) {
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;
    return same_by_contract(*a, *b);
}

}  // namespace

TEST_CASE("E-11 fuzz: equal Errors do not re-notify, unequal ones always do") {
    fuzz::Rng rng{fuzz::seed(0xE11'9A7E)};

    Property<std::optional<Error>> last_error{std::nullopt};

    std::size_t notifications = 0;
    auto sub = last_error.on_changed(
        [&notifications](const std::optional<Error>&) { ++notifications; });

    // Reference model: the value the property should currently hold.
    std::optional<Error> model = std::nullopt;
    std::size_t expected_notifications = 0;

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        std::optional<Error> next;

        switch (rng.u32(0, 3)) {
        case 0:
            // A fresh instance of a random pool entry.
            next = make_error(static_cast<int>(rng.u32(0, kPoolSize - 1)));
            break;
        case 1:
            next = std::nullopt;
            break;
        case 2:
            // Re-write what is already there, rebuilt from scratch so
            // `inner` is a different pointer. MUST NOT notify.
            next = model;
            if (next) {
                // Rebuild rather than copy, to force a new exception_ptr
                // where the entry carries one.
                for (int i = 0; i < kPoolSize; ++i) {
                    if (same_by_contract(make_error(i), *model)) {
                        next = make_error(i);
                        break;
                    }
                }
            }
            break;
        default:
            // Cleared then immediately re-set in the same step — the
            // shape a retry loop produces (E-20 clause 1 clears on
            // every execute start).
            last_error.set(std::nullopt);
            if (model.has_value()) ++expected_notifications;
            model = std::nullopt;
            next = make_error(static_cast<int>(rng.u32(0, kPoolSize - 1)));
            break;
        }

        // Reference decision: did the value change BY CONTRACT? Computed
        // from E-11's field list directly, never via the library's
        // `operator==` (see `same_by_contract`).
        const bool changes = !same_by_contract(model, next);

        // Keep the pre-write value: the library-vs-contract cross-check
        // below has to compare the OLD and NEW values, which is the pair
        // the gate itself judged. Comparing after `model = next` would
        // compare a value against its own copy — always equal, always
        // passing, and therefore worthless.
        const std::optional<Error> prev = model;

        last_error.set(next);
        if (changes) ++expected_notifications;
        model = next;

        // The gate must agree with contract equality at every step.
        CHECK(notifications == expected_notifications);

        // And the library's own `operator==` must agree with the
        // contract, which is what makes the gate correct rather than
        // merely self-consistent. This is the assertion that fails if
        // `operator==` starts comparing `inner`.
        if (prev.has_value() && next.has_value()) {
            CHECK((*prev == *next) == same_by_contract(*prev, *next));
        }
    }

    // Sanity: the walk actually exercised both branches, otherwise the
    // agreement above is vacuous.
    CHECK(expected_notifications > 0);
    CHECK(expected_notifications < fuzz::iters());
}
