#pragma once

// ============================================================================
//  aria/error.hpp
// ----------------------------------------------------------------------------
//  Unified error model for the Aria framework. Per docs/error-model.md,
//  every observable error face in Aria reports through `aria::Error`:
//
//    - reactive graph cycles      -> ErrorKind::GraphCycle
//    - validator rule failures    -> ErrorKind::Validation        + ValidationKey
//    - async command body failure -> ErrorKind::AsyncFailure
//    - async cancellation         -> ErrorKind::Cancellation
//    - async with_timeout         -> ErrorKind::Timeout
//    - view binding setter failed -> ErrorKind::BindingFailure
//    - bad user argument          -> ErrorKind::UserError
//    - internal contract broken   -> ErrorKind::InvariantViolation
//
//  Properties of the type:
//
//    * Value-typed: copyable, equality-comparable. Designed to be the
//      `T` of `Property<std::optional<Error>>`.
//
//    * Embedded ValidationKey: a ValidationError no longer needs its
//      own struct -- it is just an `Error` with `kind = Validation`
//      and a populated `key`. This keeps a single observable surface
//      (`vector<Error>`) instead of fragmenting per error family.
//
//    * Optional `source` string: a stable, free-form locator for "who
//      produced this" (e.g. "AsyncCommand", "AsyncResource", "Validator",
//      "BindingEngine"). Renderers can group / filter on it.
//
//    * Optional `inner` exception_ptr: an escape hatch for callers that
//      want full stack-trace context. Most consumers never touch it;
//      `message` carries the human-facing text already.
//
//    * `Error::from_exception(...)`: canonical mapping from a thrown
//      exception_ptr to a typed Error, recognising the framework's own
//      sentinel exception types (`OperationCancelled`, `TimeoutError`,
//      `CircularDependencyError`).
//
//  Per docs/api-style.md S-1 the type lives in `aria::` and never
//  forces the caller to qualify into an implementation namespace.
// ============================================================================

#include "aria/validation_key.hpp"

#include <cstdint>
#include <exception>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace aria {

// ---------------------------------------------------------------------------
//  ErrorKind
// ---------------------------------------------------------------------------

/// Coarse classification of every error Aria can surface. Designed so
/// that a UI router can route on `kind` alone before inspecting any
/// other field. Stable enumerator order; never re-ordered, only
/// appended at the end.
enum class ErrorKind : std::uint8_t {
    /// Caller passed something the API explicitly forbids (null view
    /// model, out-of-range index, ...). Maps `std::invalid_argument`
    /// and `std::out_of_range`.
    UserError = 0,

    /// A validator rule failed. The accompanying `Error::key` is
    /// populated; `Error::source` is "Validator" or
    /// "FormValidator".
    Validation = 1,

    /// An asynchronous body threw a non-cancellation, non-timeout
    /// exception. `Error::inner` carries the original `exception_ptr`
    /// when available.
    AsyncFailure = 2,

    /// An `OperationCancelled` propagated through the async pipeline.
    /// Cancellation is a normal control-flow path; UI typically
    /// renders nothing for this kind.
    Cancellation = 3,

    /// A `with_timeout` deadline elapsed (either Race or Fail mode).
    Timeout = 4,

    /// View / adapter side-effect failed (e.g. native widget setter
    /// threw). Currently reserved for future binding-side reporting;
    /// today binding errors propagate as exceptions.
    BindingFailure = 5,

    /// Reactive graph reached `kMaxFlushRounds` -- the dependency DAG
    /// contains a cycle. Maps `aria::CircularDependencyError`.
    GraphCycle = 6,

    /// A documented framework invariant was violated at runtime
    /// (reserved for stress / fuzz reporting; never thrown from the
    /// happy path).
    InvariantViolation = 7,
};

[[nodiscard]] inline std::string_view to_string(ErrorKind k) noexcept {
    switch (k) {
        case ErrorKind::UserError:          return "UserError";
        case ErrorKind::Validation:         return "Validation";
        case ErrorKind::AsyncFailure:       return "AsyncFailure";
        case ErrorKind::Cancellation:       return "Cancellation";
        case ErrorKind::Timeout:            return "Timeout";
        case ErrorKind::BindingFailure:     return "BindingFailure";
        case ErrorKind::GraphCycle:         return "GraphCycle";
        case ErrorKind::InvariantViolation: return "InvariantViolation";
    }
    return "ErrorKind?";
}

[[nodiscard]] inline std::string_view to_string(Severity s) noexcept {
    return s == Severity::Error ? "Error" : "Warning";
}

// ---------------------------------------------------------------------------
//  Error
// ---------------------------------------------------------------------------

/// One uniform error record. Consumed by every observable error face
/// in the framework (`AsyncCommand::last_error`,
/// `AsyncResource::error`, `ValidationState::errors`, ...).
struct Error {
    ErrorKind                       kind     = ErrorKind::AsyncFailure;
    Severity                        severity = Severity::Error;
    std::string                     message;
    /// Free-form locator: which subsystem produced this. Stable
    /// strings such as "AsyncCommand", "AsyncResource", "Validator",
    /// "FormValidator", "Graph", "Binding". Empty if unknown.
    std::string                     source;
    /// For `kind == Validation`: the `(field_path, rule_id)` locator.
    /// Empty for every other kind (still a valid `ValidationKey`,
    /// just with empty strings).
    ValidationKey                   key;
    /// Optional original exception. Most consumers never need it; it
    /// is intentionally not part of equality (see `operator==` below)
    /// because exception_ptrs are pointer-identity-compared and that
    /// would break observer change detection on no-op resets.
    std::exception_ptr              inner;

    [[nodiscard]] bool is_error()   const noexcept { return severity == Severity::Error; }
    [[nodiscard]] bool is_warning() const noexcept { return severity == Severity::Warning; }

    [[nodiscard]] bool is_cancellation() const noexcept {
        return kind == ErrorKind::Cancellation;
    }

    /// Stable single-line render: `<kind>:<source>:<key>: <message>`.
    /// Useful for log lines and GraphInspector traces.
    [[nodiscard]] std::string to_string() const {
        std::string out;
        out.append(::aria::to_string(kind));
        if (!source.empty()) { out.push_back(':'); out.append(source); }
        if (!key.empty())    { out.push_back(':'); out.append(key.to_string()); }
        out.append(": ");
        out.append(message);
        return out;
    }

    // ── Factories ─────────────────────────────────────────────────────

    /// Hard validation error. Callers usually go through
    /// `Validator`/`FormValidator`, which build these for them.
    [[nodiscard]] static Error validation(ValidationKey k, std::string msg) {
        return Error{ErrorKind::Validation, Severity::Error,
                     std::move(msg), "Validator", std::move(k), {}};
    }

    /// Soft validation advisory (`severity = Warning`).
    [[nodiscard]] static Error validation_warning(ValidationKey k, std::string msg) {
        return Error{ErrorKind::Validation, Severity::Warning,
                     std::move(msg), "Validator", std::move(k), {}};
    }

    /// Async failure with optional inner exception_ptr preserved.
    [[nodiscard]] static Error async_failure(std::string msg,
                                             std::string source_tag = "AsyncCommand",
                                             std::exception_ptr inner = {}) {
        return Error{ErrorKind::AsyncFailure, Severity::Error,
                     std::move(msg), std::move(source_tag), {}, std::move(inner)};
    }

    /// Cancellation. Always Severity::Error (consumers can opt to
    /// render it as nothing). Source tag defaults to "AsyncCommand"
    /// because that is by far the most common emitter.
    [[nodiscard]] static Error cancellation(std::string source_tag = "AsyncCommand") {
        return Error{ErrorKind::Cancellation, Severity::Error,
                     "operation cancelled", std::move(source_tag), {}, {}};
    }

    /// `with_timeout` deadline expired.
    [[nodiscard]] static Error timeout(std::string source_tag = "AsyncCommand") {
        return Error{ErrorKind::Timeout, Severity::Error,
                     "operation timed out", std::move(source_tag), {}, {}};
    }

    /// Bad caller argument (mirrors `std::invalid_argument` /
    /// `std::out_of_range`).
    [[nodiscard]] static Error user_error(std::string msg, std::string source_tag = {}) {
        return Error{ErrorKind::UserError, Severity::Error,
                     std::move(msg), std::move(source_tag), {}, {}};
    }

    /// Reactive graph cycle (mirrors `CircularDependencyError`).
    [[nodiscard]] static Error graph_cycle(std::string msg,
                                           std::exception_ptr inner = {}) {
        return Error{ErrorKind::GraphCycle, Severity::Error,
                     std::move(msg), "Graph", {}, std::move(inner)};
    }

    /// Catch-all converter from a thrown `exception_ptr` to a typed
    /// `Error`. Recognises:
    ///   * `std::invalid_argument` / `std::out_of_range` -> UserError
    ///   * `std::exception`                              -> AsyncFailure
    ///   * unknown                                       -> AsyncFailure
    ///
    /// Aria's own sentinel exception types (`OperationCancelled`,
    /// `TimeoutError`, `CircularDependencyError`) are intentionally
    /// NOT recognised here, because doing so would force `error.hpp`
    /// to back-include `async/cancellation.hpp` etc. and reverse the
    /// module dependency. Each error-face site (e.g.
    /// `aria::async::detail::process_async_exception`) catches the
    /// sentinel first and calls a precise factory
    /// (`Error::cancellation()`, `Error::timeout()`,
    /// `Error::graph_cycle()`); only the residual reaches
    /// `from_exception`.
    ///
    /// `source_tag` lets the call site annotate "where did this come
    /// from"; pick a stable string ("AsyncCommand", "AsyncResource",
    /// ...) so consumers can route on it.
    [[nodiscard]] static Error from_exception(std::exception_ptr ex,
                                              std::string source_tag) {
        if (!ex) {
            return Error::async_failure("unknown error",
                                        std::move(source_tag), {});
        }
        try {
            std::rethrow_exception(ex);
        } catch (const std::invalid_argument& e) {
            return Error::user_error(e.what(), std::move(source_tag));
        } catch (const std::out_of_range& e) {
            return Error::user_error(e.what(), std::move(source_tag));
        } catch (const std::exception& e) {
            return Error::async_failure(e.what(), std::move(source_tag), ex);
        } catch (...) {
            return Error::async_failure("unknown error",
                                        std::move(source_tag), ex);
        }
    }
};

// Equality intentionally ignores `inner` (exception_ptr identity is
// not a useful equivalence) so that re-emitting the same logical
// failure does NOT trigger Property::on_changed when wrapped in
// `Property<std::optional<Error>>`.
inline bool operator==(const Error& a, const Error& b) noexcept {
    return a.kind     == b.kind
        && a.severity == b.severity
        && a.message  == b.message
        && a.source   == b.source
        && a.key      == b.key;
}
inline bool operator!=(const Error& a, const Error& b) noexcept {
    return !(a == b);
}

inline std::ostream& operator<<(std::ostream& os, const Error& e) {
    return os << e.to_string();
}

}  // namespace aria
