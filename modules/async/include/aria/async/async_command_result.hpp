#pragma once

// AsyncCommandResult<R>
//
// Strongly-typed return value of AsyncCommand::co_execute(). Captures
// the FOUR distinct outcomes a command invocation may have, instead of
// silently collapsing them onto a default-constructed R:
//
//   * Completed  — the action ran to completion.
//                  For R != void, `value` holds the produced value.
//                  For R == void, `value` is absent (the type has no
//                  payload field).
//   * Dropped    — the command's policy is DropIfRunning AND another
//                  invocation was in flight when this one was issued,
//                  so it never started.
//   * Cancelled  — an OperationCancelled was observed before completion.
//                  This includes:
//                     - command-wide cancellation (dtor, manual cancel)
//                     - per-invocation cancellation (LatestOnly preempt)
//                     - user-initiated CancellationToken trips inside
//                       the action body
//                  `error` carries the structured cancellation Error.
//   * Failed     — the action threw a non-cancellation exception.
//                  `error` carries the mapped aria::Error (TimeoutError,
//                  domain Errors, std::exception, …).
//
// Design rationale (vs. legacy `co_return R{}` on drop):
//   * `R == 0` is a valid business value for `Task<int>`. Collapsing
//     "I didn't run" onto it is undefined-by-convention. Users had no
//     way to distinguish "search returned 0 hits" from "search dropped
//     because previous one still running".
//   * Forces R to be DefaultConstructible. AsyncCommand<MyResult> with
//     a non-default-constructible MyResult failed to compile. The new
//     model lifts that constraint entirely.
//   * The four-state enum makes UI reactions explicit. A "Save" button
//     that uses DropIfRunning genuinely wants to know "did it actually
//     save, or was I rate-limited?" — not "I got a default Foo back,
//     hope that means something".
//
// co_execute() never throws under the new contract. All exceptions —
// including OperationCancelled — fold into a status, so callers can
// write straight-line code:
//
//     auto r = co_await cmd.co_execute(query);
//     if (r) use(*r);                    // Completed
//     else if (r.dropped())   notify_busy();
//     else if (r.cancelled()) /* silently swallow, e.g. user typed */;
//     else if (r.failed())    show_error(*r.error);
//
// The fire-and-forget `execute()` path keeps the same observable
// surface as before (`is_executing`, `last_error`, `last_result`); it
// internally consumes an AsyncCommandResult too, but the user sees no
// difference.

#include "aria/error.hpp"

#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace aria::async {

/// Outcome category for a single AsyncCommand invocation.
///
/// Exactly one applies per `co_execute()` call. The four states are
/// closed: any future evolution of the command machinery must map back
/// to one of them (e.g. a future "rate-limited" policy folds into
/// `Dropped`, a "deadline-exceeded" elaboration folds into `Failed`).
enum class AsyncCommandStatus : std::uint8_t {
    Completed,   ///< action finished, value (if any) produced
    Dropped,     ///< DropIfRunning rejected this invocation; never started
    Cancelled,   ///< OperationCancelled observed before completion
    Failed,      ///< action threw a non-cancellation exception
};

namespace detail {

/// Common shape for both R and void specialisations: status flags +
/// optional structured error. Pulled into a base so the predicates
/// don't get re-typed twice.
struct AsyncCommandResultBase {
    AsyncCommandStatus           status{AsyncCommandStatus::Completed};
    std::optional<::aria::Error> error{};

    [[nodiscard]] bool completed() const noexcept { return status == AsyncCommandStatus::Completed; }
    [[nodiscard]] bool dropped()   const noexcept { return status == AsyncCommandStatus::Dropped;   }
    [[nodiscard]] bool cancelled() const noexcept { return status == AsyncCommandStatus::Cancelled; }
    [[nodiscard]] bool failed()    const noexcept { return status == AsyncCommandStatus::Failed;    }
};

}  // namespace detail

/// Result of `AsyncCommand<R, Args...>::co_execute(...)`.
///
/// Conversion to bool is `completed()` — the most common consumption
/// pattern (`if (r) use(*r);`). For richer dispatch use the explicit
/// predicates or inspect `.status` / `.error` directly.
///
/// `value` is engaged iff `status == Completed`. Dereferencing
/// (`*r` / `r->`) on a non-Completed result is undefined behaviour;
/// guard with `if (r)` first. This mirrors the std::optional contract
/// and keeps the happy path zero-overhead.
template<typename R>
struct AsyncCommandResult : detail::AsyncCommandResultBase {
    static_assert(!std::is_reference_v<R>,
        "AsyncCommandResult<R&> is not supported. Use a value type or "
        "wrap in std::reference_wrapper at the action layer.");

    std::optional<R> value{};

    [[nodiscard]] explicit operator bool() const noexcept { return completed(); }

    /// Direct access to the produced value. Precondition: `completed()`.
    /// Defined only for the four standard ref qualifiers so move-out
    /// works naturally:
    ///
    ///     auto v = std::move(*co_await cmd.co_execute(args));
    [[nodiscard]] R&        operator*() &       noexcept { return *value; }
    [[nodiscard]] const R&  operator*() const&  noexcept { return *value; }
    [[nodiscard]] R&&       operator*() &&      noexcept { return std::move(*value); }
    [[nodiscard]] const R&& operator*() const&& noexcept { return std::move(*value); }

    [[nodiscard]] R*        operator->()        noexcept { return &*value; }
    [[nodiscard]] const R*  operator->() const  noexcept { return &*value; }

    /// Convenience: extract value or fall back. Does NOT throw on
    /// non-completed; pair with explicit status checks if you need to
    /// distinguish "no value" from "default value".
    template<typename U>
    [[nodiscard]] R value_or(U&& fallback) const& {
        return completed() ? *value : static_cast<R>(std::forward<U>(fallback));
    }
    template<typename U>
    [[nodiscard]] R value_or(U&& fallback) && {
        return completed() ? std::move(*value)
                           : static_cast<R>(std::forward<U>(fallback));
    }

    // ── Factories ─────────────────────────────────────────────────
    // Used by AsyncCommand internals; exposed publicly because they
    // are the cleanest way to mock an AsyncCommandResult in user
    // tests (e.g. injecting a fake command stub).
    [[nodiscard]] static AsyncCommandResult completed_with(R v) {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Completed;
        r.value.emplace(std::move(v));
        return r;
    }
    [[nodiscard]] static AsyncCommandResult dropped_() {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Dropped;
        return r;
    }
    [[nodiscard]] static AsyncCommandResult cancelled_(::aria::Error e) {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Cancelled;
        r.error.emplace(std::move(e));
        return r;
    }
    [[nodiscard]] static AsyncCommandResult failed_(::aria::Error e) {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Failed;
        r.error.emplace(std::move(e));
        return r;
    }
};

/// Specialisation for void-returning commands. No payload field; the
/// status alone (plus optional error) carries the full outcome.
template<>
struct AsyncCommandResult<void> : detail::AsyncCommandResultBase {
    [[nodiscard]] explicit operator bool() const noexcept { return completed(); }

    [[nodiscard]] static AsyncCommandResult completed_with() {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Completed;
        return r;
    }
    [[nodiscard]] static AsyncCommandResult dropped_() {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Dropped;
        return r;
    }
    [[nodiscard]] static AsyncCommandResult cancelled_(::aria::Error e) {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Cancelled;
        r.error.emplace(std::move(e));
        return r;
    }
    [[nodiscard]] static AsyncCommandResult failed_(::aria::Error e) {
        AsyncCommandResult r;
        r.status = AsyncCommandStatus::Failed;
        r.error.emplace(std::move(e));
        return r;
    }
};

}  // namespace aria::async
