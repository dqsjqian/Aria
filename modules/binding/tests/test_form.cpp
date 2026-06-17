#include <doctest/doctest.h>

#include "aria/binding/form.hpp"

#include <string>

using namespace aria::binding;

TEST_CASE("FormField: required + min_length update validity") {
    FormField<std::string> username{"username", ""};
    username.required("username required")
            .min_length(3, "too short");

    CHECK_FALSE(username.is_valid.get());
    CHECK(username.error.get() == "username required");

    username.value = "ab";
    CHECK_FALSE(username.is_valid.get());
    CHECK(username.error.get() == "too short");

    username.value = "alice";
    CHECK(username.is_valid.get());
    CHECK(username.error.get() == "");
}

TEST_CASE("FormField: touched and dirty") {
    FormField<std::string> f{"f", "hello"};
    CHECK_FALSE(f.touched.get());
    CHECK_FALSE(f.dirty.get());

    f.value = "world";
    CHECK(f.touched.get());
    CHECK(f.dirty.get());

    f.reset("again");
    CHECK_FALSE(f.touched.get());
    CHECK_FALSE(f.dirty.get());
    CHECK(f.value.get() == "again");
}

TEST_CASE("FormGroup: aggregates validity and dirty state") {
    FormField<std::string> user{"user", ""};
    user.required("required");
    FormField<std::string> pass{"pass", ""};
    pass.min_length(6, "too short");

    FormGroup group;
    group.track(user);
    group.track(pass);

    CHECK_FALSE(group.is_valid.get());
    CHECK_FALSE(group.is_dirty.get());

    user.value = "alice";
    CHECK_FALSE(group.is_valid.get());
    CHECK(group.is_dirty.get());

    pass.value = "123456";
    CHECK(group.is_valid.get());
    CHECK(group.is_dirty.get());
}

TEST_CASE("FormGroup: clear resets aggregate") {
    FormField<std::string> user{"user", ""};
    user.required("required");

    FormGroup group;
    group.track(user);
    CHECK_FALSE(group.is_valid.get());

    group.clear();
    CHECK(group.is_valid.get());
    CHECK_FALSE(group.is_dirty.get());
}

// ── FormValidator ───────────────────────────────────────────────────────────

TEST_CASE("FormValidator: aggregate is_valid across multiple fields") {
    FormField<std::string> email{"email", ""};    email.required("email required");
    FormField<std::string> password{"password", ""}; password.required("password required");

    FormValidator form;
    form.track(email);
    form.track(password);

    CHECK_FALSE(form.is_valid.get());
    CHECK(form.first_error.get() == "email required");

    email.value = "alice@example.com";
    CHECK_FALSE(form.is_valid.get());
    CHECK(form.first_error.get() == "password required");

    password.value = "hunter2";
    CHECK(form.is_valid.get());
    CHECK(form.first_error.get() == "");
}

TEST_CASE("FormValidator: cross-field rule fires when fields disagree") {
    FormField<std::string> pwd{"pwd", ""};
    FormField<std::string> confirm{"confirm", ""};
    pwd.required("password required");
    confirm.required("confirm required");

    FormValidator form;
    form.track(pwd);
    form.track(confirm);
    form.rule([&]{ return pwd.value.get() == confirm.value.get(); },
              "passwords do not match");

    pwd.value     = "secret";
    confirm.value = "wrong";
    CHECK_FALSE(form.is_valid.get());
    CHECK(form.first_error.get() == "passwords do not match");

    confirm.value = "secret";
    CHECK(form.is_valid.get());
    CHECK(form.first_error.get() == "");
}

TEST_CASE("FormValidator: is_dirty becomes true when any field touched") {
    FormField<std::string> name{"name", "alice"};
    FormValidator form;
    form.track(name);

    CHECK_FALSE(form.is_dirty.get());
    name.value = "bob";
    CHECK(form.is_dirty.get());
}

TEST_CASE("FormValidator: clear resets all aggregate state") {
    FormField<std::string> a{"a", ""};
    a.required("required");

    FormValidator form;
    form.track(a);
    form.rule([&]{ return false; }, "rule always fails");

    CHECK_FALSE(form.is_valid.get());
    CHECK_FALSE(form.first_error.get().empty());

    form.clear();
    CHECK(form.is_valid.get());
    CHECK(form.first_error.get() == "");
    CHECK_FALSE(form.is_dirty.get());
    CHECK_FALSE(form.is_pending.get());
}

TEST_CASE("FormValidator: rule failure dominates per-field errors") {
    FormField<std::string> a{"a", "x"};   // valid (no rule)
    FormField<std::string> b{"b", ""};    // invalid via .required
    b.required("b required");

    FormValidator form;
    form.track(a);
    form.track(b);
    form.rule([&]{ return a.value.get().size() >= 5; },
              "a must be at least 5 chars");

    // Both rule and field b fail; rule wins first_error.
    CHECK_FALSE(form.is_valid.get());
    CHECK(form.first_error.get() == "a must be at least 5 chars");

    a.value = "abcde";  // rule passes; b's error surfaces.
    CHECK(form.first_error.get() == "b required");
}

TEST_CASE("FormValidator: empty form is trivially valid") {
    FormValidator form;
    CHECK(form.is_valid.get());
    CHECK_FALSE(form.is_dirty.get());
    CHECK_FALSE(form.is_pending.get());
    CHECK(form.first_error.get() == "");
}

TEST_CASE("FormValidator: rule re-evaluates when any tracked field changes") {
    FormField<int> a{"a", 1};
    FormField<int> b{"b", 2};
    FormValidator form;
    form.track(a);
    form.track(b);
    int rule_calls = 0;
    form.rule([&]{ ++rule_calls; return a.value.get() < b.value.get(); }, "a<b");

    int before = rule_calls;
    a.value = 5;
    CHECK(rule_calls > before);
    CHECK_FALSE(form.is_valid.get());

    b.value = 10;
    CHECK(form.is_valid.get());
}

TEST_CASE("FormValidator: tracking fields with non-string types still aggregates") {
    FormField<int> qty{"qty", 1};
    qty.must([](const int& v) { return v >= 0; }, "must be non-negative");

    FormValidator form;
    form.track(qty);
    CHECK(form.is_valid.get());

    qty.value = -3;
    CHECK_FALSE(form.is_valid.get());
    CHECK(form.first_error.get() == "must be non-negative");
}

TEST_CASE("FormValidator: multi-track + multi-rule aggregates correctly") {
    FormField<std::string> first{"first", "alice"};
    FormField<std::string> last{"last", "smith"};
    FormField<int>         age{"age", 30};
    FormValidator form;
    form.track(first);
    form.track(last);
    form.track(age);
    form.rule([&]{ return age.value.get() >= 18; }, "must be 18+");
    form.rule([&]{
        return !first.value.get().empty() && !last.value.get().empty();
    }, "name fields required");

    CHECK(form.is_valid.get());

    age.value = 10;
    CHECK_FALSE(form.is_valid.get());
    CHECK(form.first_error.get() == "must be 18+");

    age.value = 20;
    last.value = "";
    CHECK_FALSE(form.is_valid.get());
    CHECK(form.first_error.get() == "name fields required");
}

TEST_CASE("FormValidator: track is no-op-safe when passed already-valid field") {
    FormField<std::string> a{"a", "hello"};
    FormValidator form;
    form.track(a);
    CHECK(form.is_valid.get());
    CHECK(form.first_error.get() == "");
}

TEST_CASE("FormValidator: chained read of is_valid for submit gating") {
    FormField<std::string> email{"email", ""};
    email.required("required");

    FormValidator form;
    form.track(email);

    bool submit_enabled = false;
    auto sub = form.is_valid.bind([&](bool v) { submit_enabled = v; });

    CHECK_FALSE(submit_enabled);
    email.value = "x@y";
    CHECK(submit_enabled);
}
