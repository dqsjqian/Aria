// fuzz_validator_field_path — docs/reference/error-model.md §7:
//
//    "Validator errors' `key.field_path` always equals the Validator's path"
//
// E-22 clause 1 makes `field_path` an *invariant of the Validator*, not
// of the individual rule: whatever path the ctor was handed must appear
// on every Error the validator ever produces, no matter which surface
// produced it. There are four such surfaces and they are implemented in
// four different places inside `Validator`:
//
//   * `errors`   — a failing `rule` / `must`
//   * `warnings` — a failing `warning` / `should`
//   * `end_pending(vector<string>)` — async messages the framework
//     wraps itself (rule_id becomes "async_<N>")
//   * `end_pending(vector<Error>)`  — async errors the *caller* shaped,
//     where the framework only backfills an empty `field_path`
//
// The last one is the interesting one: the framework must fill in its
// own path when the caller left it blank, and must NOT overwrite a path
// the caller set deliberately. That asymmetry is easy to break with a
// one-line "always assign" and no test would notice, so it is fuzzed
// here alongside the rest.
//
// E-22 clause 1 also pins `rule_id`: explicit when given, else
// "rule_<N>" — checked on the same pass, since a rule_id regression
// would otherwise hide behind a passing field_path assertion.

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "aria/error.hpp"
#include "aria/property.hpp"
#include "aria/validator.hpp"
#include "fuzz_support.hpp"

namespace {

using aria::Error;
using aria::ErrorKind;
using aria::Property;
using aria::ValidationKey;
using aria::Validator;

/// Field paths worth throwing at the validator: empty (the default),
/// flat, dotted, indexed, and one with characters that a naive
/// implementation might try to normalise.
const std::vector<std::string>& paths() {
    static const std::vector<std::string> v{
        "",
        "email",
        "user.profile.email",
        "items[3].qty",
        "  spaced  ",
        "\xE4\xB8\xAD\xE6\x96\x87",   // UTF-8, must survive verbatim
    };
    return v;
}

struct ExpectedIssue {
    std::string path;
    std::string rule_id;
    std::string message;
};

// The oracle comes from generated rule inputs, never from Validator's
// output or its has_error_with_rule helper. Missing and rewritten IDs,
// wrong severity, reordered records and discarded messages must all fail.
void check_issues(const std::vector<Error>& actual,
                  const std::vector<ExpectedIssue>& expected,
                  aria::Severity severity) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(actual[i].kind == ErrorKind::Validation);
        CHECK(actual[i].severity == severity);
        CHECK(actual[i].key.field_path == expected[i].path);
        CHECK(actual[i].key.rule_id == expected[i].rule_id);
        CHECK(actual[i].message == expected[i].message);
    }
}

}  // namespace

TEST_CASE("fuzz: Validator field_path is an invariant of the validator") {
    auto rng = aria::fuzz::Rng{aria::fuzz::seed(0xE2'20'F1'1D)};
    const std::size_t iters = aria::fuzz::iters();

    for (std::size_t i = 0; i < iters; ++i) {
        const std::string path =
            paths()[rng.u32(0, static_cast<std::uint32_t>(paths().size() - 1))];

        Property<std::string> value{"seed"};
        Validator<std::string> v{value, path};

        // ── Sync surfaces: a mix of always-failing and always-passing
        // rules and warnings, with and without an explicit rule_id.
        const int n_rules = static_cast<int>(rng.u32(0, 3));
        std::vector<ExpectedIssue> expected_errors;
        std::vector<ExpectedIssue> expected_warnings;
        for (int r = 0; r < n_rules; ++r) {
            const bool fails    = rng.u32(0, 1) == 1;
            const bool explicit_id = rng.u32(0, 1) == 1;
            const bool as_warning  = rng.u32(0, 1) == 1;

            std::string id;
            if (explicit_id) {
                id = "rid_" + std::to_string(r);
            }
            if (fails) {
                auto& expected = as_warning ? expected_warnings : expected_errors;
                expected.push_back({path, explicit_id ? id : "rule_" + std::to_string(r), "failed"});
            }

            auto body = [fails](const std::string&) -> std::optional<std::string> {
                if (fails) return std::string{"failed"};
                return std::nullopt;
            };
            if (as_warning) {
                v.warning(body, id);
            } else {
                v.rule(body, id);
            }
        }

        check_issues(v.state().peek().errors, expected_errors, aria::Severity::Error);
        check_issues(v.state().peek().warnings, expected_warnings, aria::Severity::Warning);

        // ── Async surface. Alternate between the three `end_pending`
        // overloads so all of them are exercised over the run.
        const std::uint32_t which = rng.u32(0, 2);
        v.begin_pending();
        CHECK(v.state().peek().pending);

        if (which == 0) {
            v.end_pending();
        } else if (which == 1) {
            // Framework-shaped: it wraps raw strings itself.
            const std::size_t n = rng.u32(0, 2);
            std::vector<std::string> msgs;
            for (std::size_t m = 0; m < n; ++m) {
                msgs.push_back("async_" + std::to_string(m));
                expected_errors.push_back({path, "async_" + std::to_string(m), msgs.back()});
            }
            v.end_pending(std::move(msgs));
        } else {
            // Caller-shaped: half the time the caller leaves field_path
            // empty (framework must backfill), half the time it sets its
            // own (framework must NOT clobber it).
            const bool caller_sets_path = rng.u32(0, 1) == 1;
            std::vector<Error> supplied;
            const std::size_t n = rng.u32(1, 2);
            for (std::size_t m = 0; m < n; ++m) {
                std::string own = caller_sets_path ? "caller/own/path" : std::string{};
                supplied.push_back(Error::validation(
                    ValidationKey{own, "caller_rule_" + std::to_string(m)},
                    "caller supplied"));
                expected_errors.push_back({caller_sets_path ? own : path,
                                            "caller_rule_" + std::to_string(m), "caller supplied"});
            }
            v.end_pending(std::move(supplied));
        }

        CHECK_FALSE(v.state().peek().pending);

        const auto settled = v.state().peek();

        check_issues(settled.errors, expected_errors, aria::Severity::Error);
        check_issues(settled.warnings, expected_warnings, aria::Severity::Warning);
        CHECK(settled.valid == expected_errors.empty());

        // `field_path()` itself never drifts, whatever happened above.
        CHECK(v.field_path() == path);

        // A later source write re-runs the rules; the path must hold
        // across a revalidation too.
        //
        // Async records remain until the next settlement. Their caller
        // paths, IDs and messages must survive this rerun unchanged, so
        // the same independently generated expectation still applies.
        value.set("changed-" + std::to_string(i));
        const auto rerun = v.state().peek();
        check_issues(rerun.errors, expected_errors, aria::Severity::Error);
        check_issues(rerun.warnings, expected_warnings, aria::Severity::Warning);
        CHECK(rerun.valid == expected_errors.empty());
        CHECK(v.field_path() == path);
    }
}
