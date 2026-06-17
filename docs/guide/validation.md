# Validation

Aria's validation system is reactive-first: validators bind to `Property<T>` and automatically re-evaluate when the value changes. Results are exposed as `Property<ValidationState>` — bind them to your UI like any other reactive value.

**Include:** `#include "aria/validator.hpp"`

---

## Validator\<T\>

Attaches validation rules to a `Property<T>`. Re-runs all rules whenever the source changes.

### Basic: Required Field

```cpp
aria::Property<std::string> email{""};

aria::Validator<std::string> email_valid{email, "email"};
email_valid.must([](const std::string& v) { return !v.empty(); },
                 "Email is required", "required");

email_valid.state().bind([](const aria::ValidationState& s) {
    if (!s.valid) {
        show_error(s.first_error_message().value_or(""));
    }
});
```

### Multiple Rules

```cpp
email_valid
    .must([](const std::string& v) { return !v.empty(); },
           "Email is required", "required")
    .must([](const std::string& v) { return v.contains('@'); },
           "Must contain @", "at_sign")
    .must([](const std::string& v) { return v.size() >= 5; },
           "Too short", "min_length");
```

Rules are evaluated in order. All failing rules contribute errors — the user sees every problem at once.

### Warnings (Soft Advisories)

Warnings do NOT make the field invalid:

```cpp
email_valid.should([](const std::string& v) { return !v.ends_with(".co"); },
                    ".co domains may have issues", "dot_co_warning");
```

### Custom Rule Function

Full control — return `std::optional<std::string>` (message on failure, `nullopt` on pass):

```cpp
email_valid.rule([](const std::string& v) -> std::optional<std::string> {
    if (v.find("..") != std::string::npos)
        return "Consecutive dots are not allowed";
    return std::nullopt;
}, "no_double_dots");
```

### Bulk Rules

Add multiple rules without triggering intermediate evaluations:

```cpp
email_valid.rules({
    [](const std::string& v) -> std::optional<std::string> {
        return v.empty() ? std::optional{"Required"} : std::nullopt;
    },
    [](const std::string& v) -> std::optional<std::string> {
        return !v.contains('@') ? std::optional{"Need @"} : std::nullopt;
    }
});
```

---

## ValidationState

The rich form-state record exposed by `validator.state()`:

| Field | Type | Meaning |
|-------|------|---------|
| `valid` | `bool` | True if no Error-severity entries |
| `pending` | `bool` | True while async validation is running |
| `touched` | `bool` | True after user interaction (focus-out) |
| `dirty` | `bool` | True if value differs from baseline |
| `errors` | `vector<Error>` | Hard failures |
| `warnings` | `vector<Error>` | Soft advisories |

### Convenience Queries

```cpp
auto& s = validator.state().get();

s.first_error();            // optional<Error>
s.first_error_message();    // optional<string>
s.errors_for("email");      // errors targeting a specific field
s.has_error_with_rule("required");  // bool
```

---

## Form-State Transitions

### Touch

Mark a field as user-interacted (typically on blur):

```cpp
email_valid.touch();
// state().touched → true
```

### Reset Dirty

Snap the baseline to the current value (e.g. after a successful save):

```cpp
email_valid.reset_dirty();
// state().dirty → false
```

### Reset Touched

Clear the touched flag (e.g. when resetting a form):

```cpp
email_valid.reset_touched();
// state().touched → false
```

---

## Async Validation

For rules that need server-side checks (username availability, etc.):

```cpp
aria::Validator<std::string> username_valid{username, "username"};

username_valid.must([](const std::string& v) { return v.size() >= 3; },
                    "At least 3 characters", "min_length");

// In your async handler:
username_valid.begin_pending();  // state().pending → true

// ... later, when server responds ...
username_valid.end_pending({"Username taken"});  // pending → false, adds async errors
// Or:
username_valid.end_pending();  // pending → false, no extra errors
```

---

## ValidationResult (Legacy)

A simpler projection with just `valid` + `errors`. Available via `validator.result()`:

```cpp
auto& r = email_valid.result().get();
r.valid;              // bool
r.errors;             // vector<Error>
r.error_messages();   // vector<string>
```

New code should prefer `state()` — it includes `pending`, `touched`, and `dirty`.

---

## FormValidator

Compose multiple validators into a form-level validity check:

```cpp
#include "aria/binding/form.hpp"

aria::binding::FormValidator form;

form.add(email_valid);
form.add(password_valid);
form.add(age_valid);

form.state().bind([](const aria::ValidationState& s) {
    submit_button.setEnabled(s.valid && !s.pending);
});
```

---

## Quick Reference

| Method | Description |
|--------|-------------|
| `Validator(prop, field_path)` | Attach to a Property |
| `.rule(fn, id)` | Add custom rule |
| `.must(pred, msg, id)` | Add predicate rule (error) |
| `.should(pred, msg, id)` | Add predicate rule (warning) |
| `.rules({...})` | Bulk-add rules |
| `.touch()` | Mark as user-interacted |
| `.reset_touched()` | Clear touched flag |
| `.reset_dirty()` | Snap baseline to current |
| `.begin_pending()` | Enter async-validation state |
| `.end_pending(msgs)` | Exit pending, add async errors |
| `.state()` | `Property<ValidationState>&` |
| `.result()` | `Property<ValidationResult>&` |

---

## See Also

- [Reactive Core →](reactive-core.md) — `Property` that validators consume
- [Error Model →](../reference/error-model.md) — `Error`, `ErrorKind`, unified error taxonomy
- [ViewModel →](viewmodel.md) — validators as ViewModel members
