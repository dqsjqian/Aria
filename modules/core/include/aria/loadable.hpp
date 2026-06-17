// ============================================================================
//  aria/loadable.hpp
// ----------------------------------------------------------------------------
//  `Loadable<T>` -- the standard "loadable view-model" sum type, used
//  to present an asynchronous resource to the UI without each call
//  site having to hand-roll a state machine over (is_loading, data,
//  error). Designed by analogy with:
//
//      SwiftUI:    Result<T, Error> / async-let pattern
//      Compose:    LoadState (Loading / NotLoading / Error)
//      RxJava:     Observable<Result<T>>
//      Apollo:     useQuery -> { loading, data, error, ... }
//
//  Aria's flavour preserves the framework's two strict properties:
//
//    1. Errors are always `aria::Error` (not std::exception_ptr or
//       string). The kind / source / key fields are visible to the
//       view-model so it can route validation vs async failures vs
//       cancellations differently.
//    2. Cancellation is NEVER surfaced as `Error`. A cancelled
//       operation collapses back to `Loading` (or `Idle` if there
//       was no prior data) -- it is not a result.
//
//  Five states (LO-1):
//
//      Idle        -- nothing requested yet, no data, no error
//      Loading     -- first fetch in flight (no prior data)
//      Refreshing  -- subsequent fetch in flight while a prior
//                     successful value is still on display
//                     (stale-while-revalidate). `value()` returns the
//                     stale-but-shown value.
//      Success     -- fetch completed, value held
//      Error       -- fetch failed. May still expose the last good
//                     value via `value()` (SWR), distinct from
//                     `error()`.
//
//  The five states are kept in a single tagged enum + payload struct
//  rather than a `std::variant`, because variant<monostate, T, T,
//  Error> would erase the Loading-vs-Refreshing distinction and force
//  callers to look at multiple Properties. With a tag we keep the
//  state machine the single source of truth.
// ============================================================================
#pragma once

#include "aria/error.hpp"

#include <optional>
#include <type_traits>
#include <utility>

namespace aria {

/// Discriminator for `Loadable<T>`. See file-level docs for the
/// semantic of each tag (LO-1).
enum class LoadState : unsigned char {
    Idle       = 0,
    Loading    = 1,
    Refreshing = 2,
    Success    = 3,
    Error      = 4,
};

/// Standard loadable view-model. Stores up to one value (the freshest
/// known good) and up to one error (the last failure). Combined with
/// `state()` they encode the five-state view-model.
///
/// The class is value-typed and equality-comparable so it composes
/// cleanly with `Property<Loadable<T>>`'s equality-gated set semantics
/// (per `lifecycle.md` L-21 and `error-model.md` E-11). It holds no
/// resources of its own.
template<class T>
class Loadable {
public:
    using value_type = T;

    // ── Factories (LO-2) -----------------------------------------------
    [[nodiscard]] static Loadable idle() noexcept { return {}; }

    [[nodiscard]] static Loadable loading() {
        Loadable l;
        l.state_ = LoadState::Loading;
        return l;
    }

    /// Build a `Refreshing` state from an existing `Success` payload.
    /// `prior` is the value still on display while the refresh runs.
    [[nodiscard]] static Loadable refreshing(T prior) {
        Loadable l;
        l.state_ = LoadState::Refreshing;
        l.value_ = std::move(prior);
        return l;
    }

    [[nodiscard]] static Loadable success(T v) {
        Loadable l;
        l.state_ = LoadState::Success;
        l.value_ = std::move(v);
        return l;
    }

    /// Build an `Error` state. If `prior` is provided the loadable
    /// keeps the last good value visible (stale-while-revalidate).
    [[nodiscard]] static Loadable error(Error err) {
        Loadable l;
        l.state_ = LoadState::Error;
        l.error_ = std::move(err);
        return l;
    }
    [[nodiscard]] static Loadable error(Error err, T prior) {
        Loadable l;
        l.state_ = LoadState::Error;
        l.error_ = std::move(err);
        l.value_ = std::move(prior);
        return l;
    }

    // ── Predicates (LO-3) ----------------------------------------------
    [[nodiscard]] LoadState state() const noexcept { return state_; }

    [[nodiscard]] bool is_idle()       const noexcept { return state_ == LoadState::Idle; }
    [[nodiscard]] bool is_loading()    const noexcept { return state_ == LoadState::Loading; }
    [[nodiscard]] bool is_refreshing() const noexcept { return state_ == LoadState::Refreshing; }
    [[nodiscard]] bool is_success()    const noexcept { return state_ == LoadState::Success; }
    [[nodiscard]] bool is_error()      const noexcept { return state_ == LoadState::Error; }

    /// True iff a fetch is currently in flight (Loading or Refreshing).
    /// Mirrors AsyncResource's `is_loading` Property.
    [[nodiscard]] bool in_flight() const noexcept {
        return state_ == LoadState::Loading || state_ == LoadState::Refreshing;
    }

    /// True iff the loadable currently has a value to show
    /// (Success, Refreshing, or Error+prior).
    [[nodiscard]] bool has_value() const noexcept {
        return value_.has_value();
    }

    /// True iff an error is currently surfaced. Only true in `Error`
    /// state (Loading/Refreshing/Success never carry an error per
    /// `error-model.md` -- a fresh fetch clears the previous error).
    [[nodiscard]] bool has_error() const noexcept {
        return state_ == LoadState::Error && error_.has_value();
    }

    // ── Accessors (LO-4) -----------------------------------------------
    /// Returns a pointer to the value if `has_value()`, else nullptr.
    [[nodiscard]] const T* value() const noexcept {
        return value_.has_value() ? &*value_ : nullptr;
    }

    /// Returns the value or `fallback` if absent. Never throws.
    template<class U>
    [[nodiscard]] T value_or(U&& fallback) const {
        return value_.has_value() ? *value_ : T{std::forward<U>(fallback)};
    }

    /// Returns a pointer to the error if `has_error()`, else nullptr.
    [[nodiscard]] const Error* error() const noexcept {
        return has_error() ? &*error_ : nullptr;
    }

    // ── Equality (LO-5) ------------------------------------------------
    //
    // Loadable<T> is equality-comparable iff T is. This is required
    // for `Property<Loadable<T>>` to drop redundant `set()` writes
    // per L-21 / E-11.
    friend bool operator==(const Loadable& a, const Loadable& b) {
        if (a.state_ != b.state_) return false;
        if (a.value_.has_value() != b.value_.has_value()) return false;
        if constexpr (std::equality_comparable<T>) {
            if (a.value_.has_value() && *a.value_ != *b.value_) return false;
        }
        if (a.error_.has_value() != b.error_.has_value()) return false;
        if (a.error_.has_value() && !(*a.error_ == *b.error_)) return false;
        return true;
    }
    friend bool operator!=(const Loadable& a, const Loadable& b) {
        return !(a == b);
    }

    // ── Functor / monad-ish helpers (LO-6) -----------------------------
    //
    // `map` projects the value when present, leaving the state /
    // error untouched. The error type does NOT change because Aria
    // already pins it to `aria::Error` (one error model framework-wide).
    template<class F>
    [[nodiscard]] auto map(F&& f) const
        -> Loadable<std::remove_cvref_t<std::invoke_result_t<F, const T&>>>
    {
        using U = std::remove_cvref_t<std::invoke_result_t<F, const T&>>;
        Loadable<U> out;
        out.set_state_(state_);
        if (value_.has_value()) {
            out.set_value_(std::invoke(std::forward<F>(f), *value_));
        }
        if (error_.has_value()) {
            out.set_error_(*error_);
        }
        return out;
    }

private:
    template<class> friend class Loadable;

    void set_state_(LoadState s) noexcept { state_ = s; }
    template<class U>
    void set_value_(U&& v) { value_.emplace(std::forward<U>(v)); }
    void set_error_(Error e) { error_.emplace(std::move(e)); }

    LoadState            state_{LoadState::Idle};
    std::optional<T>     value_{};
    std::optional<Error> error_{};
};

}  // namespace aria
