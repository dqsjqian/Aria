#include <doctest/doctest.h>
#include <unordered_map>

#include "aria/property.hpp"
#include "aria/validator.hpp"
#include <string>
#include <unordered_map>

using namespace aria;

TEST_CASE("Validator: passes when no rules") {
    Property<std::string> name("Alice");
    Validator<std::string> v(name);
    CHECK(v.result().get().valid);
    CHECK(v.result().get().errors.empty());
}

TEST_CASE("Validator: rules report errors") {
    Property<std::string> name("");
    Validator<std::string> v(name);
    v.must([](const std::string& s) { return !s.empty(); }, "must not be empty")
     .must([](const std::string& s) { return s.size() <= 20; }, "max 20 chars");

    auto r = v.result().get();
    CHECK_FALSE(r.valid);
    CHECK(r.errors.size() == 1);
    CHECK(r.errors[0].message == "must not be empty");
}

TEST_CASE("Validator: re-runs on source change") {
    Property<std::string> name("");
    Validator<std::string> v(name);
    v.must([](const std::string& s) { return !s.empty(); }, "must not be empty");

    CHECK_FALSE(v.result().get().valid);
    name = "Bob";
    CHECK(v.result().get().valid);
    name = "";
    CHECK_FALSE(v.result().get().valid);
}

// ═══════════════════════════════════════════════════════════════════════
//  ValidationState — form-state facets (touched / dirty / warnings / pending)
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("ValidationState: touched is false until touch() is called") {
    Property<std::string> name("");
    Validator<std::string> v(name);
    v.must([](const std::string& s) { return !s.empty(); }, "required");

    CHECK_FALSE(v.state().get().touched);
    CHECK_FALSE(v.state().get().valid);    // invalid but not yet surfaced

    v.touch();
    CHECK(v.state().get().touched);

    // Idempotent.
    v.touch();
    CHECK(v.state().get().touched);

    v.reset_touched();
    CHECK_FALSE(v.state().get().touched);
}

TEST_CASE("ValidationState: dirty tracks baseline") {
    Property<std::string> name("initial");
    Validator<std::string> v(name);
    CHECK_FALSE(v.state().get().dirty);

    name = "edited";
    CHECK(v.state().get().dirty);

    // Restoring the baseline clears dirty.
    name = "initial";
    CHECK_FALSE(v.state().get().dirty);

    // reset_dirty adopts the current value as the new baseline.
    name = "saved";
    CHECK(v.state().get().dirty);
    v.reset_dirty();
    CHECK_FALSE(v.state().get().dirty);
    name = "saved";        // equals new baseline
    CHECK_FALSE(v.state().get().dirty);
    name = "edited-again";
    CHECK(v.state().get().dirty);
}

TEST_CASE("ValidationState: warnings don't flip valid") {
    Property<std::string> pw("hi");
    Validator<std::string> v(pw);
    v.must([](const std::string& s) { return !s.empty(); }, "required")
     .should([](const std::string& s) { return s.size() >= 8; }, "short passwords are weak");

    auto s = v.state().get();
    CHECK(s.valid);                        // required rule passed
    CHECK(s.errors.empty());
    REQUIRE(s.warnings.size() == 1);
    CHECK(s.warnings[0].message == "short passwords are weak");

    // Legacy `result()` still reports valid (warnings excluded).
    CHECK(v.result().get().valid);
}

TEST_CASE("ValidationState: first_error exposes the first message") {
    Property<std::string> s("");
    Validator<std::string> v(s);
    v.must([](const std::string& x) { return !x.empty(); }, "required")
     .must([](const std::string& x) { return x.size() > 2; }, "too short");

    auto st = v.state().get();
    REQUIRE(st.first_error().has_value());
    CHECK(st.first_error()->message == "required");
    CHECK(st.first_error_message() == std::optional<std::string>{"required"});
}

TEST_CASE("ValidationState: pending round-trip with async error") {
    Property<std::string> user("alice");
    Validator<std::string> v(user);
    v.must([](const std::string& s) { return !s.empty(); }, "required");

    CHECK_FALSE(v.state().get().pending);

    v.begin_pending();
    CHECK(v.state().get().pending);
    CHECK(v.state().get().valid);          // no errors yet

    // Async check came back positive → "username taken".
    v.end_pending(std::vector<std::string>{"username already taken"});
    const auto st = v.state().get();
    CHECK_FALSE(st.pending);
    CHECK_FALSE(st.valid);
    REQUIRE(st.errors.size() == 1);
    CHECK(st.errors[0].message == "username already taken");

    // Clearing the async error on a subsequent round.
    v.begin_pending();
    v.end_pending();
    CHECK(v.state().get().valid);
    CHECK(v.state().get().errors.empty());
}

TEST_CASE("ValidationState: observable via Property<ValidationState>") {
    Property<std::string> name("");
    Validator<std::string> v(name);
    v.must([](const std::string& s) { return !s.empty(); }, "required");

    int notifications = 0;
    auto sub = v.state().on_changed([&](const ValidationState&) { ++notifications; });

    v.touch();                   // 1 notification (touched flip)
    name = "Alice";              // 2: dirty flip + valid flip coalesced into
                                 //    however many set() the run_ path emits
    CHECK(notifications >= 1);   // at least one observable update happened
}

// ═══════════════════════════════════════════════════════════════════════
//  ValidationKey protocol
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("ValidationKey: rule_id explicit + auto-generated") {
    Property<std::string> email("");
    Validator<std::string> v(email, "user.email");
    v.must([](const std::string& s) { return !s.empty(); }, "required", "required")
     .must([](const std::string& s) { return s.find('@') != std::string::npos; },
           "must contain @");                       // auto-id

    auto st = v.state().get();
    REQUIRE(st.errors.size() == 2);

    CHECK(st.errors[0].kind           == ErrorKind::Validation);
    CHECK(st.errors[0].key.field_path == "user.email");
    CHECK(st.errors[0].key.rule_id    == "required");
    CHECK(st.errors[0].message        == "required");
    CHECK(st.errors[0].is_error());

    CHECK(st.errors[1].kind           == ErrorKind::Validation);
    CHECK(st.errors[1].key.field_path == "user.email");
    CHECK(st.errors[1].key.rule_id    == "rule_1");   // auto-generated
    CHECK(st.errors[1].message        == "must contain @");
}

TEST_CASE("ValidationKey: errors_for and has_error_with_rule queries") {
    Property<std::string> name("");
    Validator<std::string> v(name, "user.name");
    v.must([](const std::string& s) { return !s.empty(); }, "required", "required")
     .must([](const std::string& s) { return s.size() >= 3; }, "too short", "min_length");

    auto st = v.state().get();
    CHECK(st.errors_for("user.name").size() == 2);
    CHECK(st.errors_for("user.email").empty());
    CHECK(st.has_error_with_rule("required"));
    CHECK(st.has_error_with_rule("min_length"));
    CHECK_FALSE(st.has_error_with_rule("max_length"));
}

TEST_CASE("ValidationKey: warning carries severity = Warning, not Error") {
    Property<std::string> pw("hi");
    Validator<std::string> v(pw, "user.password");
    v.should([](const std::string& s) { return s.size() >= 8; },
             "short passwords are weak",
             "weak");

    auto st = v.state().get();
    REQUIRE(st.warnings.size() == 1);
    CHECK(st.warnings[0].is_warning());
    CHECK(st.warnings[0].kind           == ErrorKind::Validation);
    CHECK(st.warnings[0].key.field_path == "user.password");
    CHECK(st.warnings[0].key.rule_id    == "weak");
    CHECK(st.valid);   // warnings do not flip valid
}

TEST_CASE("ValidationKey: async error backfills empty field_path with validator's") {
    Property<std::string> user("alice");
    Validator<std::string> v(user, "user.username");

    // Caller-supplied Error with custom rule_id but no path:
    // run_() should fill in `field_path` from the validator.
    v.end_pending(std::vector<Error>{
        Error::validation({"", "username_taken"}, "already taken"),
    });

    auto st = v.state().get();
    REQUIRE(st.errors.size() == 1);
    CHECK(st.errors[0].key.field_path == "user.username");
    CHECK(st.errors[0].key.rule_id    == "username_taken");
    CHECK(st.errors[0].kind           == ErrorKind::Validation);
}

TEST_CASE("ValidationKey: field/rule_id DSL produces equal keys") {
    using namespace aria;
    auto k1 = field("user.email") / rule_id("required");
    ValidationKey k2{"user.email", "required"};
    CHECK(k1 == k2);
    CHECK(k1.to_string() == "user.email#required");
}

TEST_CASE("ValidationKey: hashable for use as unordered_map key") {
    std::unordered_map<ValidationKey, std::string> bucket;
    bucket[{"user.email", "required"}] = "Email is required";
    bucket[{"user.email", "format"  }] = "Email format invalid";
    CHECK(bucket.size() == 2);
    CHECK(bucket[{"user.email", "required"}] == "Email is required");
}

TEST_CASE("ValidationResult: error_messages projection preserves order") {
    Property<std::string> name("");
    Validator<std::string> v(name, "user.name");
    v.must([](const std::string& s) { return !s.empty(); }, "first")
     .must([](const std::string& s) { return s.size() > 0; }, "second");

    auto msgs = v.result().get().error_messages();
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[0] == "first");
    CHECK(msgs[1] == "second");
}
