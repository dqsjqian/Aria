# Recipe 2 — Cross-field rule

**Goal:** validate a constraint that spans *two* fields — "password ==
confirm-password", or "start date ≤ end date" — and surface it as a
single form-level error, not anchored to either field.

`FormValidator::rule(predicate, message, rule_id)` is exactly this: the
predicate is a `bool()` closure that may read any number of Properties.
The emitted `Error` has an empty `field_path` and `source =
"FormValidator"` (so it routes as a form-level message, not a per-field
one), keyed by `ValidationKey{"", rule_id}`.

```cpp
#include "aria/binding/form.hpp"
#include "aria/property.hpp"

aria::Property<std::string> password{""};
aria::Property<std::string> confirm{""};

aria::FormValidator form;

// Cross-field: passwords must match.
form.rule(
    [&]{ return password.get() == confirm.get(); },
    "Passwords do not match",
    "password_match");

// Another cross-field rule on a date range.
aria::Property<int> start_day{1};
aria::Property<int> end_day{1};
form.rule(
    [&]{ return start_day.get() <= end_day.get(); },
    "Start must be on or before end",
    "date_order");
```

The rule re-evaluates whenever any tracked field changes (wire the
fields with `form.track(field)` so their `on_changed` feeds
`recompute_()`), and the first failing rule populates `form.first_error`
/ `form.first_error_full`:

```cpp
auto err_sub = form.first_error.on_changed([](const std::string& msg){
    banner.text    = msg;
    banner.visible = !msg.empty();
});
```

## Notes

- **Order matters for the headline.** `first_error` reports the *first*
  failing rule in registration order — register the most important rule
  first if you want it to win the banner.
- **Keep predicates pure and cheap.** They run on every change of any
  tracked field; do not perform I/O here (use an `AsyncValidator` —
  Recipe 1 — for that).
- **Routing.** Because the error's `field_path` is empty and `source ==
  "FormValidator"`, a diagnostics filter on `source` cleanly separates
  form-level rules from per-field ones (see
  `docs/reference/diagnostics.md`).
