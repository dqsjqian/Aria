# Recipe 7 — View-destroy cancellation

**Goal:** when the user navigates away from a sub-view *mid-request*,
cancel the in-flight async work so it never resumes against a destroyed
view. This is the third lifetime axis (see
`docs/reference/lifecycle.md` L-38.1; ROADMAP P1-H).

Aria already cancels in-flight work on **VM destroy**
(`ViewModelScope`) and **Navigator entry pop**. The gap this recipe
closes is the case where *neither* fires — a sub-view inside a
still-living page.

## The primitive

`BindingEngine::bind_view_lifetime(view, on_destroy_cb)` runs
`on_destroy_cb` exactly once, when the view is destroyed **or** the
engine is cleared/destroyed — whichever comes first. It lives in the
`binding` module, which deliberately does **not** depend on
`aria-async`, so you wire the async side yourself in one explicit line
(no hidden coupling).

## Cancelling an AsyncCommand

```cpp
#include "aria/binding/binding_engine.hpp"
#include "aria/async/async_command.hpp"
using namespace aria;
using namespace aria::async;

AsyncCommand<void> load{ui_executor,
    [](CancellationToken tok) -> Task<void> {
        co_await fetch_page(tok);          // cooperative cancel point
    }};

engine.bind_command(load.trigger(), view);                 // click → execute
engine.bind_view_lifetime(view, [&load]{
    load.cancel_all_in_flight();                           // view gone → cancel
});
```

Now leaving the page mid-request flips the in-flight invocation's
`CancellationToken`; the coroutine unwinds at its next probe (after a
`schedule_on` hop) instead of resuming to write into a dead view.

## Cancelling an AsyncResource

`AsyncResource::cancel()` drops the in-flight write-back, re-arms a fresh
`CancellationSource` (the resource stays usable on re-mount), and keeps
the last `data` visible (stale-while-revalidate):

```cpp
AsyncResource<Profile, int> profile{ui_executor, net_pool, fetch_profile};

engine.bind_view_lifetime(view, [&profile]{ profile.cancel(); });
```

If the user comes back to the sub-view later, a fresh `fetch()` /
`refresh()` works exactly as before — the previous value was preserved,
so the view re-renders instantly while the new fetch runs.

## Guarantees (L-38.1)

- **Fires exactly once**, on the UI thread that tears the view down.
- **Idempotent with engine teardown**: if the engine is destroyed before
  the view, the callback still fires once (so you never leak the
  cancellation hook).
- **No use-after-free**: even without the hook, a late coroutine writes
  into `shared_ptr`-owned state, not the view — the hook is about
  *stopping wasted work and bad UX*, layered on top of memory safety
  that already holds.

## Testing it

`test_binding_engine.cpp` pins the primitive (fires on view destroy /
on engine clear / exactly once). `test_async_resource.cpp` pins
`cancel()` (drops in-flight write-back, re-arms, preserves last data).
For the full destroy-race stress, see the
`binding_view_destroy_race` fuzzer (L-32).
