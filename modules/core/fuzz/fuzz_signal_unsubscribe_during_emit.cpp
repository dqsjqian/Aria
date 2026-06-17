// ============================================================================
//  fuzz_signal_unsubscribe_during_emit.cpp  (L-13)
// ----------------------------------------------------------------------------
//  Invariant under stress:
//    "abi::SignalErased uses snapshot-then-invoke. A slot disconnected
//     during an emit MUST still be invoked for THAT emit (snapshot
//     semantics) but never for any subsequent one. New slots added
//     during emit MUST NOT be invoked for the current emit."
//
//  Strategy:
//    - Maintain an arbitrary number of `Subscription` handles.
//    - Each emit randomly: (a) disconnects an existing handle from
//      inside one of the handlers, (b) adds a new slot from inside
//      one of the handlers. Both operations are checked against the
//      snapshot semantics by counting expected vs observed
//      invocations.
//    - Cross-iteration: total observed invocations must equal the
//      sum of "live slots at emit start" plus 0 for slots added mid
//      emit (by the snapshot rule).
// ============================================================================

#include <doctest/doctest.h>

#include "aria/detail/typed_signal.hpp"
#include "fuzz_support.hpp"

#include <memory>
#include <vector>

using namespace aria;

TEST_CASE("L-13 fuzz: unsubscribe-during-emit + add-during-emit snapshot rules") {
    aria::detail::TypedSignal<int> sig;
    fuzz::Rng rng{fuzz::seed(0xA1A1'1313)};

    std::vector<Subscription> subs;
    std::size_t per_handler_calls = 0;

    auto make_handler = [&] {
        return [&](const int&) noexcept { ++per_handler_calls; };
    };

    // Seed with a few slots so the very first emit is non-trivial.
    for (int i = 0; i < 4; ++i) subs.push_back(sig.connect(make_handler()));

    // Each emit records: live count at start (= snapshot size) and
    // the number of dispatches the handlers actually saw.
    for (std::size_t step = 0; step < fuzz::iters(); ++step) {
        const std::size_t snapshot_size = sig.slot_count();
        std::size_t before = per_handler_calls;

        // Two perturbations driven by handlers themselves: emit a
        // sentinel value that triggers BOTH a random disconnect AND
        // a random add inside the very first handler call. We do
        // this by installing a one-shot orchestrator subscription at
        // the head of the slot list (so it fires inside the
        // snapshot) that mutates `subs` mid emit.
        bool disconnect_happened = false;
        bool add_happened = false;

        Subscription orchestrator = sig.connect(
            [&](const int&) noexcept {
                // Randomly drop one existing subscription from the
                // user list (NOT the orchestrator itself).
                if (!subs.empty() && rng.coin(0.5)) {
                    const std::size_t i = rng.u32(0, static_cast<std::uint32_t>(subs.size() - 1));
                    subs[i].release();   // disconnect mid-emit
                    subs.erase(subs.begin() + static_cast<std::ptrdiff_t>(i));
                    disconnect_happened = true;
                }
                // Randomly add a new subscription mid-emit. Per L-13
                // the new slot MUST NOT be invoked for THIS emit.
                if (rng.coin(0.5)) {
                    subs.push_back(sig.connect(make_handler()));
                    add_happened = true;
                }
            });

        // The orchestrator we just installed is itself a snapshot
        // member (it was added BEFORE this emit started). It is
        // counted in `snapshot_size + 1`.
        const std::size_t expected = snapshot_size + 1;
        sig.emit(static_cast<int>(step));
        const std::size_t observed = per_handler_calls - before;

        // Per L-13: snapshot semantics. The number of user-handler
        // invocations equals the snapshot size (i.e. all handlers
        // alive at emit START). Disconnect mid-emit does NOT skip
        // an already-snapshotted slot; add mid-emit does NOT add a
        // dispatch. Hence: observed == snapshot_size, regardless of
        // whether disconnect/add fired during the emit.
        CHECK(observed == snapshot_size);
        (void)expected;
        (void)disconnect_happened;
        (void)add_happened;

        orchestrator.release();   // drop the orchestrator before next emit

        // Sanity: live slot count never goes negative or wildly
        // unbounded; cap subs to bound state.
        if (subs.size() > 32) subs.resize(32);
    }
}
