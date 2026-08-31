// ============================================================================
//  fuzz_error_from_exception_table.cpp  (E-13 / E-12)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "Error::from_exception(exception_ptr, source) is a DEGRADED mapping
//     that recognises only standard library exceptions:
//
//        std::invalid_argument -> UserError
//        std::out_of_range     -> UserError
//        other std::exception  -> AsyncFailure (inner retained)
//        unknown               -> AsyncFailure('unknown error')
//
//     Aria's own sentinel exceptions are NOT recognised here."
//
//  Why fuzz a lookup table: the mapping is implemented as an ordered
//  chain of catch clauses, and catch order is load-bearing —
//  `std::invalid_argument` derives from `std::logic_error` which derives
//  from `std::exception`, so moving the generic clause above the
//  specific ones silently reclassifies UserError as AsyncFailure. A
//  single call per type would catch that, but not the properties that
//  only show up over many calls:
//
//    * STABILITY: the same exception type must map to the same kind
//      every time, in any interleaving order. A table built with a
//      static/cached lookup could drift.
//    * `inner` RETENTION follows the factory each branch calls, not the
//      exception's richness (see the catalogue comment) — and Errors
//      carrying DIFFERENT exception_ptr values must still compare equal
//      (E-11), which is the property the Property write gate depends on.
//    * E-12: every produced Error carries the requested source.
//
//  Strategy: throw a randomly chosen type from a fixed catalogue,
//  capture it, map it, and assert kind / source / inner against the
//  catalogue entry. Interleaving is random so no per-type ordering can
//  be assumed.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/error.hpp"
#include "fuzz_support.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aria;

namespace {

/// One catalogue entry: how to raise it, and what E-13 promises.
struct Case {
    const char* label;
    ErrorKind   expected_kind;
    bool        expects_inner;      ///< inner retained?
    bool        message_is_what;    ///< message == e.what()?
    void (*raise)();
};

// Raising through free functions keeps each throw site distinct and
// avoids any chance of the compiler folding them together.
void raise_invalid_argument() { throw std::invalid_argument("bad-arg"); }
void raise_out_of_range()     { throw std::out_of_range("oor"); }
void raise_runtime_error()    { throw std::runtime_error("boom"); }
void raise_logic_error()      { throw std::logic_error("logic"); }
void raise_bad_alloc()        { throw std::bad_alloc{}; }
void raise_non_std()          { throw 42; }
void raise_non_std_struct()   { struct Alien { int c; }; throw Alien{7}; }

const std::vector<Case>& catalogue() {
    // `std::invalid_argument` and `std::out_of_range` are the two
    // specifically recognised types; everything else that is a
    // std::exception degrades to AsyncFailure; a non-std throw keeps
    // the exception_ptr but loses the message.
    //
    // `expects_inner` follows the *factory* each branch calls, not the
    // exception's own richness — which is counter-intuitive enough to be
    // worth pinning:
    //
    //   * `Error::user_error(msg, source)` has no `inner` parameter at
    //     all, so the two UserError branches DROP the exception_ptr even
    //     though `from_exception` was handed one. Verified against
    //     error.hpp: user_error constructs with `{}` in the inner slot.
    //   * `catch (...)` calls `async_failure("unknown error", tag, ex)`
    //     — it forwards `ex`. So an `int` throw, which carries the least
    //     information of all, still ends up WITH an inner ptr.
    //
    // Anyone "tidying up" this asymmetry would change observable
    // behaviour; that is what these two columns exist to catch.
    static const std::vector<Case> cases = {
        {"std::invalid_argument", ErrorKind::UserError,    false, true,  &raise_invalid_argument},
        {"std::out_of_range",     ErrorKind::UserError,    false, true,  &raise_out_of_range},
        {"std::runtime_error",    ErrorKind::AsyncFailure, true,  true,  &raise_runtime_error},
        {"std::logic_error",      ErrorKind::AsyncFailure, true,  true,  &raise_logic_error},
        {"std::bad_alloc",        ErrorKind::AsyncFailure, true,  true,  &raise_bad_alloc},
        {"int (non-std)",         ErrorKind::AsyncFailure, true,  false, &raise_non_std},
        {"struct (non-std)",      ErrorKind::AsyncFailure, true,  false, &raise_non_std_struct},
    };
    return cases;
}

/// Capture a raised exception as an exception_ptr.
std::exception_ptr capture(void (*raise)()) {
    try {
        raise();
    } catch (...) {
        return std::current_exception();
    }
    return {};
}

/// `what()` of an exception_ptr, or empty for a non-std payload.
std::string what_of(const std::exception_ptr& ep) {
    try {
        std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return {};
    }
}

}  // namespace

TEST_CASE("E-13 fuzz: from_exception mapping is stable across std exception types") {
    fuzz::Rng rng{fuzz::seed(0xE13'7AB'1E)};

    const auto& cases = catalogue();
    const std::string source = "FuzzSurface";

    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const auto& c = cases[rng.u32(0, static_cast<std::uint32_t>(cases.size() - 1))];

        auto ep = capture(c.raise);
        REQUIRE(ep != nullptr);

        const auto err = Error::from_exception(ep, source);

        // E-13: the mapping itself.
        CHECK(err.kind == c.expected_kind);
        // E-12: source is always carried through.
        CHECK(err.source == source);
        // `inner` retained only for std::exception payloads.
        CHECK(static_cast<bool>(err.inner) == c.expects_inner);

        if (c.message_is_what) {
            CHECK(err.message == what_of(ep));
        } else {
            // Unknown payloads get a fixed placeholder rather than an
            // empty string — an empty message on an error face is
            // useless to a UI.
            CHECK_FALSE(err.message.empty());
        }

        // Mapping the SAME throw a second time must be identical: the
        // classification cannot depend on call order or on any cached
        // state inside error.hpp.
        const auto again = Error::from_exception(ep, source);
        CHECK(again.kind == err.kind);
        CHECK(again.message == err.message);
        CHECK(again.source == err.source);

        // E-11: two Errors from DISTINCT throws of the same type carry
        // different exception_ptr values yet must compare equal, since
        // `inner` is excluded from equality. This is exactly what the
        // Property write-equality gate relies on — if it broke, every
        // repeated failure would re-notify observers.
        auto ep2 = capture(c.raise);
        REQUIRE(ep2 != nullptr);
        const auto err2 = Error::from_exception(ep2, source);
        if (c.expects_inner) {
            CHECK(err.inner != err2.inner);      // genuinely different ptrs
        }
        CHECK(err == err2);                      // still equal by value
    }
}
