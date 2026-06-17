# Recipe 1 — Form with sync + async rules

**Goal:** a login/signup field that validates synchronously (non-empty,
length, format) and also runs an asynchronous rule (e.g. "username
available?") without blocking the UI.

See also: `docs/guide/validation.md`, `docs/reference/error-model.md`,
and the validation contract `V-N` in the headers.

## Synchronous rules

`FormValidator::rule(predicate, message, rule_id)` attaches a predicate
that runs on every change of the bound Property. A failing rule surfaces
its `message` under the given `rule_id`.

```cpp
#include "aria/binding/form.hpp"   // aria::FormValidator
#include "aria/property.hpp"

aria::Property<std::string> username{""};

aria::FormValidator form;
form.rule([&]{ return !username.get().empty(); },
          "Username is required", "required");
form.rule([&]{ return username.get().size() >= 3; },
          "At least 3 characters", "min_len");

// The aggregate validity is observable (a Property<bool> member, not a
// method); bind it to a submit button.
auto sub = form.is_valid.on_changed([](bool ok){ submit_button.enabled = ok; });
// Likewise: form.is_dirty, form.is_pending, form.first_error.
```

## Asynchronous rule

`aria::async::AsyncValidator<T>` runs a coroutine rule on a worker
executor and writes the pending/valid/invalid state back on the UI
executor. Its factory has the signature
`std::function<Task<AsyncRuleResult>(T value, CancellationToken tok)>`
(see `aria/async/async_validator.hpp` for `AsyncRuleResult`). It attaches
to a sync `aria::Validator<T>` plus its source `aria::Property<T>`:

```cpp
#include "aria/async/async_validator.hpp"

aria::Validator<std::string> username_validator;  // the sync validator the field uses

aria::async::AsyncValidator<std::string> uniqueness{
    ui_executor, net_pool,
    [](std::string name, aria::CancellationToken tok)
        -> aria::async::Task<aria::async::AsyncRuleResult> {
        co_return co_await api::check_username_free(std::move(name), tok);
    }};

// `attach_to` returns a Subscription that owns the wiring; dropping it
// detaches AND cancels any in-flight rule (V-6).
auto async_sub = uniqueness.attach_to(username_validator, username);
```

## Why this is safe

- **No torn state.** The async rule cancels the previous in-flight run on
  every new keystroke and mints a fresh `CancellationSource` (V-1), so a
  slow earlier request can never overwrite a newer result.
- **Pending is observable.** `attach_to` immediately fires
  `begin_pending()` (V-2), so the UI can show a spinner while the network
  rule resolves.
- **Lifetime is RAII.** Destroying the returned `Subscription` cancels
  the in-flight rule and detaches (V-6) — no callback fires against a
  dead form.
