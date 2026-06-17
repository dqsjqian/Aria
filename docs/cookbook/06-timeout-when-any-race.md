# Recipe 6 — Async `with_timeout` + `when_any` race

**Goal:** bound a network call by a deadline, and/or race several sources
and take whichever answers first — cancelling the losers.

See `aria/async/timeout.hpp` and `aria/async/when_all.hpp`
(`when_any`), and the race contract notes in those headers.

## Bound a call by a timeout

`with_timeout(timer, duration, factory, mode)` runs `factory` and races
it against a timer. `timer` is any `IDelayedScheduler` (e.g.
`VirtualTimeExecutor` in tests, a real UI timer in production). The
token-accepting factory form lets the inner work cancel cooperatively
when the deadline wins:

```cpp
#include "aria/async/timeout.hpp"
using namespace aria::async;

Task<std::string> fetch_user(int id) {
    // OnTimeout::Fail -> throws TimeoutError when the deadline expires.
    auto profile = co_await with_timeout(
        timer, std::chrono::seconds{3},
        [id](CancellationToken tok) -> Task<std::string> {
            co_return co_await api::get_profile(id, tok);  // tok flips on timeout
        },
        OnTimeout::Fail);
    co_return profile;
}
```

- **`OnTimeout::Fail`** (above) surfaces a `TimeoutError`
  (`ErrorKind::Timeout`) the moment the deadline elapses.
- **`OnTimeout::Race`** (the default) resolves with whichever of
  inner/timer finishes first without throwing — use it for
  "best-effort observe" flows.
- The **plain factory** form `with_timeout(timer, dur, []{ return work(); })`
  has no token, so the inner work cannot be cancelled — it just keeps
  running in the background after the deadline. The header flags this as
  a footgun; prefer the token-accepting form for anything cancellable.
- A **parent token** overload `with_timeout(parent, timer, dur, factory)`
  gives parent-cancel priority over the timeout.

## Race several sources — first wins, losers cancelled

`when_any_cancellable` takes factories that each receive a
`CancellationToken`; once a winner emerges, every loser's token is
flipped so it can stop cooperatively:

```cpp
#include "aria/async/when_all.hpp"

std::vector<std::function<Task<Quote>(CancellationToken)>> sources = {
    [](CancellationToken t){ return vendor_a(t); },
    [](CancellationToken t){ return vendor_b(t); },
    [](CancellationToken t){ return vendor_c(t); },
};
Quote best = co_await when_any_cancellable(std::move(sources));
// The two slower vendors had their tokens cancelled the instant the
// fastest returned.
```

The simpler `when_any(std::vector<Task<T>>)` form races tasks that have
no token attached — losers run to completion in the background. Use the
cancellable form whenever the losing work is expensive or has side
effects.

## Composing timeout + retry

`async/async_command_builder.hpp` ships `action_with_timeout` and
`action_with_retry` that wrap these combinators for `AsyncCommand`
actions, so a command can be "each attempt gets its own 3s timeout, up
to 3 attempts" without hand-rolling the race (see the header for the
exact wrapper signatures).
