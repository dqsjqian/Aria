// ============================================================================
//  test_error_model.cpp
// ----------------------------------------------------------------------------
//  Pin down the unified error-model contracts spelled out in
//  docs/error-model.md (E-N). Each TEST_CASE references its canonical
//  invariant ID so a failure points the reader straight at the
//  authoritative description.
// ============================================================================

#include <doctest/doctest.h>

#include "aria/error.hpp"
#include "aria/property.hpp"
#include "aria/validator.hpp"

#include <stdexcept>

using namespace aria;

// ============================================================================
//  E-1 / E-2: ErrorKind taxonomy + stable enumerators
// ============================================================================

TEST_CASE("E-1: ErrorKind enumerators are stable and complete") {
    // Stability contract: numeric values never change. If this test
    // forces an update because the enum was reordered, that is a
    // breaking change and CHANGELOG must record it explicitly.
    CHECK(static_cast<int>(ErrorKind::UserError)          == 0);
    CHECK(static_cast<int>(ErrorKind::Validation)         == 1);
    CHECK(static_cast<int>(ErrorKind::AsyncFailure)       == 2);
    CHECK(static_cast<int>(ErrorKind::Cancellation)       == 3);
    CHECK(static_cast<int>(ErrorKind::Timeout)            == 4);
    CHECK(static_cast<int>(ErrorKind::BindingFailure)     == 5);
    CHECK(static_cast<int>(ErrorKind::GraphCycle)         == 6);
    CHECK(static_cast<int>(ErrorKind::InvariantViolation) == 7);
}

TEST_CASE("E-1: to_string covers every ErrorKind enumerator") {
    CHECK(to_string(ErrorKind::UserError)          == "UserError");
    CHECK(to_string(ErrorKind::Validation)         == "Validation");
    CHECK(to_string(ErrorKind::AsyncFailure)       == "AsyncFailure");
    CHECK(to_string(ErrorKind::Cancellation)       == "Cancellation");
    CHECK(to_string(ErrorKind::Timeout)            == "Timeout");
    CHECK(to_string(ErrorKind::BindingFailure)     == "BindingFailure");
    CHECK(to_string(ErrorKind::GraphCycle)         == "GraphCycle");
    CHECK(to_string(ErrorKind::InvariantViolation) == "InvariantViolation");
}

// ============================================================================
//  E-10 / E-11: Error is PropertyValue; equality ignores `inner`
// ============================================================================

TEST_CASE("E-10: Error is usable as the T of Property<optional<Error>>") {
    Property<std::optional<Error>> p{std::nullopt};
    int hits = 0;
    auto sub = p.on_changed([&](const auto&) { ++hits; });

    p.set(Error::async_failure("boom", "X"));
    CHECK(hits == 1);
}

TEST_CASE("E-11: equality ignores `inner` exception_ptr") {
    auto a = Error::async_failure("boom", "X");
    auto b = Error::async_failure("boom", "X");
    // Plant different inner exception_ptrs on each:
    a.inner = std::make_exception_ptr(std::runtime_error{"inner-a"});
    b.inner = std::make_exception_ptr(std::runtime_error{"inner-b"});
    CHECK(a == b);   // E-11: inner does NOT participate
}

TEST_CASE("E-11: Property equality-gate suppresses identical Error sets") {
    Property<std::optional<Error>> p{std::nullopt};
    int hits = 0;
    auto sub = p.on_changed([&](const auto&) { ++hits; });

    auto e1 = Error::async_failure("boom", "X");
    auto e2 = Error::async_failure("boom", "X");
    e1.inner = std::make_exception_ptr(std::runtime_error{"a"});
    e2.inner = std::make_exception_ptr(std::runtime_error{"b"});

    p.set(e1);
    CHECK(hits == 1);
    p.set(e2);   // logically same error -> no observer fire
    CHECK(hits == 1);
}

// ============================================================================
//  E-13: from_exception mapping table
// ============================================================================

TEST_CASE("E-13: from_exception maps invalid_argument to UserError") {
    auto ex = std::make_exception_ptr(std::invalid_argument{"bad arg"});
    auto err = Error::from_exception(ex, "Test");
    CHECK(err.kind    == ErrorKind::UserError);
    CHECK(err.source  == "Test");
    CHECK(err.message == "bad arg");
}

TEST_CASE("E-13: from_exception maps out_of_range to UserError") {
    auto ex = std::make_exception_ptr(std::out_of_range{"oor"});
    auto err = Error::from_exception(ex, "Test");
    CHECK(err.kind == ErrorKind::UserError);
}

TEST_CASE("E-13: from_exception maps generic std::exception to AsyncFailure") {
    auto ex = std::make_exception_ptr(std::runtime_error{"net down"});
    auto err = Error::from_exception(ex, "Net");
    CHECK(err.kind     == ErrorKind::AsyncFailure);
    CHECK(err.source   == "Net");
    CHECK(err.message  == "net down");
    CHECK(err.inner != nullptr);   // inner exception_ptr is preserved
}

TEST_CASE("E-13: from_exception with null exception_ptr falls back gracefully") {
    auto err = Error::from_exception(nullptr, "X");
    CHECK(err.kind   == ErrorKind::AsyncFailure);
    CHECK(err.source == "X");
}

// ============================================================================
//  E-22: Validator-produced errors are kind = Validation, with key
// ============================================================================

TEST_CASE("E-22: Validator emits kind = Validation with populated key") {
    Property<std::string> name("");
    Validator<std::string> v(name, "user.name");
    v.must([](const std::string& s) { return !s.empty(); }, "required", "required");

    auto st = v.state().get();
    REQUIRE(st.errors.size() == 1);
    CHECK(st.errors[0].kind            == ErrorKind::Validation);
    CHECK(st.errors[0].severity        == Severity::Error);
    CHECK(st.errors[0].source          == "Validator");
    CHECK(st.errors[0].key.field_path  == "user.name");
    CHECK(st.errors[0].key.rule_id     == "required");
    CHECK(st.errors[0].message         == "required");
}

TEST_CASE("E-22: warning rules emit Severity::Warning, not Error") {
    Property<std::string> pw("hi");
    Validator<std::string> v(pw, "user.password");
    v.should([](const std::string& s) { return s.size() >= 8; },
             "weak password", "weak");

    auto st = v.state().get();
    REQUIRE(st.warnings.size() == 1);
    CHECK(st.warnings[0].severity == Severity::Warning);
    CHECK(st.warnings[0].kind     == ErrorKind::Validation);
    CHECK(st.valid);   // warnings do not flip valid
}

// ============================================================================
//  Factories: each one stamps the documented (kind, severity, source)
// ============================================================================

TEST_CASE("Factory: Error::cancellation has Cancellation kind") {
    auto e = Error::cancellation();
    CHECK(e.kind     == ErrorKind::Cancellation);
    CHECK(e.severity == Severity::Error);
    CHECK(e.is_cancellation());
}

TEST_CASE("Factory: Error::timeout has Timeout kind") {
    auto e = Error::timeout("AsyncResource");
    CHECK(e.kind   == ErrorKind::Timeout);
    CHECK(e.source == "AsyncResource");
}

TEST_CASE("Factory: Error::user_error has UserError kind") {
    auto e = Error::user_error("nope", "Navigator");
    CHECK(e.kind   == ErrorKind::UserError);
    CHECK(e.source == "Navigator");
}

TEST_CASE("Factory: Error::graph_cycle has GraphCycle kind") {
    auto e = Error::graph_cycle("loop");
    CHECK(e.kind   == ErrorKind::GraphCycle);
    CHECK(e.source == "Graph");
}

TEST_CASE("Error::to_string round-trips kind/source/key") {
    auto e = Error::validation({"user.email", "required"}, "Email is required");
    auto s = e.to_string();
    // Single-line render: "<kind>:<source>:<key>: <message>"
    CHECK(s.find("Validation")        != std::string::npos);
    CHECK(s.find("Validator")         != std::string::npos);
    CHECK(s.find("user.email#required") != std::string::npos);
    CHECK(s.find("Email is required") != std::string::npos);
}
