# Migrating from 1.2.x to 2.0

Version 2.0 changes the native ABI and several lifetime/event contracts.
Rebuild the application, plugins and adapters together; do not load ABI 1
and ABI 2 libraries into the same Aria object graph.

## Build and link

Continue linking the exported CMake targets (`aria::core`, `aria::binding`,
or an adapter target). They supply the new compiled `aria_abi` foundation
transitively. `find_package(aria REQUIRED COMPONENTS core)` checks that
requested components are present; adapter components include `qt6`, `http`,
`appkit`, `uikit` and `jni`. A core-only consumer no longer needs runtime services to
resolve graph, diagnostics or scheduler symbols. Shared builds must deploy
the ABI library as well as the other libraries they use.

The shared reactive graph supports dynamic libraries built with compatible
compiler, standard library and build settings. ABI version matching does not
make arbitrary compiler or standard-library combinations compatible. Static
builds remain available; independently linked static copies in plugins do
not form a shared graph.

C++23 is supported; C++20 remains the default and minimum. Select C++23
with `-DCMAKE_CXX_STANDARD=23`; see the
[evaluation](cpp23-evaluation.md). The unsupported WASM build option has
been removed from the implemented platform surface.

## List event consumers

`ListChange<T>::item` is now a `shared_ptr<T>`, and every Reset event carries
a non-null `snapshot`. Retain these handles when queueing an event. Use
`item.get()` only when a non-owning pointer is needed for immediate access.

Apply events to the receiver's own row mirror in order. The producer may
already contain the complete result of a range operation, so `source.at(index)`
cannot recover an intermediate event's payload. Reentrant edits follow the
current event batch. New subscriptions skip events whose mutations were
already committed before they subscribed.

Sorted views emit Moves when moving source rows changes the order of
equivalent keys. They preserve stable source order after every source edit.
Repeated shared item handles remain separate occurrences and receive their
own ItemChanged events.

## Subscriptions and dispatch

Replace `Subscription::detach()` with `release()`. Releasing a registration
skips callbacks that have not started, including later callbacks in the
current fanout. An already running callback may finish. Subscription bags
release in reverse registration order.

`main_dispatcher()` returns a `shared_ptr<IDispatcher>` snapshot. Retain it
while using the dispatcher. `SimpleDispatcher::pump()` and `run_one()` run
on its creating thread; worker threads post work instead.

Mutable converters and registered factories keep their state across calls.
Code that previously depended on a fresh callable copy per invocation must
make that reset explicit. `inplace_function` accepts copy-constructible
targets; use move-only-capable subscription/callback surfaces where needed.

## HTTP and browser clients

The browser SDK separates value, visibility and enabled channels. Replace
`subscribe("view.enabled", callback)` with
`subscribe("view", callback, "enabled")`, and use
`getState("view", "enabled")` for its cached value. Dots remain ordinary
characters in view IDs. Unsubscribe functions are idempotent and own
independent registrations, even when callbacks are reused.

Protocol 2 preserves native integer precision. The SDK returns `number` for
safe integers and `bigint` for larger values; the wire representation uses
decimal strings for the latter. Update rendering and JSON serialization
accordingly. Unsafe JavaScript numeric inputs are rejected.

Explicit HTTP worker counts start at two. SSE admission reserves capacity
for ordinary requests, and queued notifications and each SSE client's data
have finite bounds. See the [HTTP guide](guide/adapters/http.md) for limits,
failure responses and the complete integration example.

The [changelog](../CHANGELOG.md#unreleased) lists the associated fixes and
new APIs; the [lifecycle contract](reference/lifecycle.md) defines threading
and destruction requirements.
