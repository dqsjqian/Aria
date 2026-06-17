#pragma once

// race_slot.hpp — shared race-aware state primitive for awaiter-style
// coroutine combinators that resolve the parent from one of N signaller
// paths.
//
// Used by:
//   * with_timeout (OnTimeout::Fail): inner-done vs timer-fire race.
//   * when_any:                       N task-done race.
//
// Two-phase publication protocol (hardened after a peer-review
// found a publication race in the original single-atomic design):
//
//   Phase 1 — CLAIM (atomic CAS on `claimed_`)
//     Signallers race to flip `claimed_` from 0 to a unique non-zero
//     code via `try_claim(code)`. Exactly one signaller wins; that
//     signaller becomes the sole writer for the result slot.
//
//   Phase 2 — WRITE-then-PUBLISH (winner-only, ordered)
//     The winner populates `result` (and optionally `winner_index`),
//     THEN calls `publish(code)` to release-store the same code into
//     `winner_`. Only `winner_` is observed by the parent; readers
//     using acquire-load on `winner_` are guaranteed to see the
//     fully-written `result` (acquire/release synchronises-with).
//
//   Phase 3 — RESUME (mu-serialised parent_handle handoff)
//     The winner finally calls `notify_winner_resume()`, which under
//     `mu_` reads `parent_handle` and resumes it (or no-ops if the
//     parent has not yet stored its handle — the parent's
//     `await_suspend` tail will re-check `winner_` under `mu_` and
//     skip suspension).
//
// Why two atomics instead of one:
//   The original single-atomic protocol stored the winner code BEFORE
//   `result` was written, so a parent observing `winner_ != 0` via
//   `await_ready()` (which does NOT take `mu_`) could rush into
//   `await_resume()` and read an unpublished `result` — UB. Splitting
//   into "claim" (writer-resource lock, NOT observed by parent) and
//   "winner/published" (release-store AFTER result is written, observed
//   by parent under acquire) closes that window without changing the
//   public awaiter API.
//
// Why `mu_` is still needed:
//   `mu_` does NOT serialise result publication — that is now done by
//   acquire/release on `winner_`. `mu_` serialises the orthogonal
//   handle-handoff race: a signaller publishing before the parent has
//   stored its handle, vs the parent storing its handle after a winner
//   already published. Under `mu_` exactly one of {publisher.resume,
//   parent.skip-suspend} fires.
//
// Loser disengagement:
//   Late signallers (whose `try_claim` returns false) MUST silently drop
//   their result and exception. Combinators built on top of RaceSlot
//   are responsible for keeping any detached driver coroutines alive
//   long enough that this drop is safe (typically by capturing the
//   shared_ptr<RaceSlot> into the driver coroutine frame).

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

namespace aria::async::detail {

/// Shared race state. `R` is the result type; `void` is supported via
/// std::monostate in the value slot.
///
/// Two atomics, two phases:
///   * `claimed_` — Phase 1 writer-resource CAS. Internal only;
///                  signallers go through `try_claim()`. Parent never
///                  observes this directly.
///   * `winner_`  — Phase 2 published-result flag. Release-stored by
///                  the winning signaller AFTER `result` is fully
///                  written; acquire-loaded by the parent in
///                  `await_ready` / `await_suspend`. Non-zero ⇒
///                  `result` is safe to read.
template<typename R>
struct RaceSlot {
    std::mutex                 mu;
    std::atomic<int>           claimed{0};       // phase 1: writer CAS, internal
    std::atomic<int>           winner{0};        // phase 2: published flag, observed by parent
    std::coroutine_handle<>    parent_handle{};
    bool                       parent_stored = false;

    // Variant arms:
    //   index 0 = monostate (unresolved)
    //   index 1 = value     (or monostate for void R)
    //   index 2 = exception_ptr
    using ValueSlot = std::conditional_t<
        std::is_void_v<R>,
        std::variant<std::monostate, std::monostate, std::exception_ptr>,
        std::variant<std::monostate, R,             std::exception_ptr>>;
    ValueSlot result;

    /// Optional caller-side payload (e.g. when_any's winning index).
    /// Written by the winner BEFORE `publish()` so it is covered by the
    /// same release-store and visible to the parent under acquire.
    std::size_t winner_index = std::size_t(-1);

    /// Phase 1: try to claim writer resource. Returns true iff the
    /// caller is THE winner. The caller is then OBLIGATED to:
    ///   1. populate `result` (and optionally `winner_index`),
    ///   2. call `publish(code)` to release the published flag,
    ///   3. call `notify_winner_resume()` to hand off to the parent.
    /// Steps must be in this order; reordering reopens the publication
    /// race the two-phase protocol exists to close.
    bool try_claim(int code) noexcept {
        int expected = 0;
        return claimed.compare_exchange_strong(expected, code,
                                               std::memory_order_acq_rel);
    }

    /// Phase 2: publish the resolved code. MUST be called AFTER
    /// `result` (and `winner_index` if used) are written. The
    /// release-store synchronises-with the parent's acquire-load in
    /// `await_ready` / `await_suspend`, guaranteeing the parent sees a
    /// fully-written `result` once it observes a non-zero `winner`.
    void publish(int code) noexcept {
        winner.store(code, std::memory_order_release);
    }

    /// Phase 3: resume parent if its handle is already stored.
    /// Otherwise the parent's `await_suspend` tail will detect
    /// `winner != 0` under `mu_` and skip suspension.
    void notify_winner_resume() {
        std::coroutine_handle<> h{};
        {
            std::lock_guard lk(mu);
            if (parent_stored) {
                h = parent_handle;
                parent_handle = {};       // hand off; one-shot
                parent_stored = false;
            }
        }
        if (h) h.resume();
    }
};

/// Awaiter that parks the parent until a winner publishes a result.
/// Reads `winner` (the published flag) — never `claimed`.
/// `await_suspend` re-checks `winner` under `mu` and returns false to
/// skip suspension when a signaller raced ahead of us; the acquire-load
/// pairs with the winner's release-store in `publish()`, so observing
/// `winner != 0` means `result` is fully written.
template<typename R>
struct RaceSlotAwaiter {
    std::shared_ptr<RaceSlot<R>> slot;

    bool await_ready() const noexcept {
        return slot->winner.load(std::memory_order_acquire) != 0;
    }

    bool await_suspend(std::coroutine_handle<> caller) noexcept {
        std::lock_guard lk(slot->mu);
        if (slot->winner.load(std::memory_order_acquire) != 0) {
            // A signaller already published the result; skip suspend.
            return false;
        }
        slot->parent_handle = caller;
        slot->parent_stored = true;
        return true;
    }

    /// Decode the resolved result. Throws iff index 2 is engaged.
    /// Safe to call once `winner.load(acquire) != 0` has been observed.
    R await_resume() {
        auto& v = slot->result;
        if (v.index() == 2) {
            std::rethrow_exception(std::get<2>(v));
        }
        if constexpr (std::is_void_v<R>) {
            return;
        } else {
            return std::move(std::get<1>(v));
        }
    }
};

}  // namespace aria::async::detail
