#pragma once

// SortedList<T> — a live-updating sorted view over an ObservableList<T>.
//
// Given a source list and a comparator, SortedList exposes the items of
// the source, reordered to satisfy `comparator(a, b)` as a strict weak
// ordering. Equivalent items (neither `cmp(a,b)` nor `cmp(b,a)` is
// true) retain their source order — SortedList is a STABLE sort, and
// stable at runtime too: no insert, replace, or ItemChanged ever
// permutes two already-equivalent items against each other.
//
// Like FilteredList, every change to the source is translated into the
// minimal incremental sequence of derived events; small source
// mutations never escalate to a full Reset.
//
// Usage:
//
//     auto source = std::make_shared<ObservableList<Task>>();
//     auto sorted = std::make_shared<SortedList<Task>>(
//         source,
//         [](const Task& a, const Task& b) {
//             return a.priority() < b.priority();
//         });
//
//     source->push_back(std::make_shared<Task>(/*prio=*/5));
//     source->push_back(std::make_shared<Task>(/*prio=*/2));
//     // sorted sees: Insert(0), Insert(0)   — the prio=2 ends up at index 0.
//
//     source->at(0)->set_priority(10);
//     // if Task::on_changed propagates, sorted sees Move(0, 1)
//     // (NOT Remove+Insert — Move carries semantic "same object,
//     //  different position").
//
// Event-translation contract (mandatory; pinned by tests):
//
//     Source event            Derived behaviour
//     ────────────────        ──────────────────────────────────────────
//     Insert(i, x)            binary_search → Insert(j, x)  (always 1 event)
//     Remove(i)               Remove(j)                     (always 1 event)
//     Replace(i, x_new)       d_new == d_old → Replace(d_old, x_new)
//                             otherwise      → Remove(d_old), Insert(d_new)
//     ItemChanged(i)          d_new == d_old → ItemChanged(d_old)
//                             otherwise      → Move(d_old, d_new, x)
//     Move(from, to)          no derived event — derived ordering is
//                             purely a function of (comparator,
//                             source_index). We merely re-index the
//                             internal d2s / s2d maps so future events
//                             resolve to the right source slots.
//     Reset                   Reset
//
// set_comparator() reshuffles the derived list against the new
// comparator and emits a single Reset event (rather than trying to
// emit a minimal-edit sequence — that would be O(n log n) to compute
// and almost always dominated by the re-sort itself). The `reset`
// event is the honest signal "layout changed wholesale".
//
// Stability contract:
//   Equivalent items (by the comparator) are ordered by ascending
//   source index. When a source Move changes indices, the derived
//   positions of already-equivalent items stay put — see the Move
//   handling above.
//
// Thread-safety:
//   Same policy as FilteredList: internal `shared_mutex`, source
//   listener holds only weak_ptrs, emits happen after the mutation is
//   visible but before the lock is re-taken by a later event.

#include "aria/inplace_function.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/list_signal_mixin.hpp"
#include "aria/detail/typed_signal.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace aria {

template<typename T>
class SortedList
    : public detail::ListSignalMixin<SortedList<T>, T> {
    friend detail::ListSignalMixin<SortedList<T>, T>;

public:
    using value_type = T;
    /// Strict weak ordering on items. Must be stable across calls for
    /// the lifetime of the SortedList (or until `set_comparator`).
    /// Owning, heap-free comparator handle (capacity 32 bytes).
    using Comparator = aria::inplace_function<bool(const T&, const T&), 32>;
    using Signal     = detail::TypedSignal<ListChange<T>>;

    SortedList(std::shared_ptr<ObservableList<T>> source,
               Comparator comparator)
        : source_(std::move(source)),
          signal_(std::make_shared<Signal>()),
          state_(std::make_shared<SharedState>())
    {
        state_->comparator = std::move(comparator);

        // Build the initial mapping from the source snapshot. See the
        // FilteredList constructor for why we snapshot before
        // subscribing; concurrent source mutations that arrive
        // mid-construction are then delivered as normal listener
        // callbacks on top of the known-good baseline.
        {
            std::unique_lock lk(state_->m);
            rebuild_from_snapshot_unlocked_(*state_, source_->snapshot());
        }

        // Subscribe on the source. The lambda holds only weak_ptrs.
        std::weak_ptr<SharedState>       weak_state  = state_;
        std::weak_ptr<Signal>            weak_signal = signal_;
        std::weak_ptr<ObservableList<T>> weak_source{source_};
        source_sub_ = source_->observe(
            [weak_state, weak_signal, weak_source](const ListChange<T>& ch) {
                auto st  = weak_state.lock();
                auto sig = weak_signal.lock();
                auto src = weak_source.lock();
                if (!st || !sig || !src) return;
                dispatch_source_change_(*st, *sig, *src, ch);
            });
    }

    ~SortedList() = default;

    SortedList(const SortedList&)            = delete;
    SortedList& operator=(const SortedList&) = delete;

    // ── Read surface ──────────────────────────────────────────────────
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lk(state_->m);
        return state_->items.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] std::shared_ptr<T> at(std::size_t derived_index) const {
        std::shared_lock lk(state_->m);
        return state_->items.at(derived_index);
    }

    [[nodiscard]] std::vector<std::shared_ptr<T>> snapshot() const {
        std::shared_lock lk(state_->m);
        return state_->items;
    }

    [[nodiscard]] std::optional<std::size_t>
    source_index_of(std::size_t derived_index) const {
        std::shared_lock lk(state_->m);
        if (derived_index >= state_->derived_to_source.size()) return std::nullopt;
        return state_->derived_to_source[derived_index];
    }

    // ── Comparator replacement ────────────────────────────────────────
    //
    // Re-sorts the derived layout against the new comparator and emits
    // a single Reset. An O(n log n) minimal-edit sequence could be
    // computed, but the cost would dominate the re-sort itself and the
    // wider Qt/AppKit adapter ecosystem handles Reset cleanly anyway.
    void set_comparator(Comparator new_comparator) {
        {
            std::unique_lock lk(state_->m);
            state_->comparator = std::move(new_comparator);
            rebuild_from_snapshot_unlocked_(*state_, source_->snapshot());
        }
        signal_->emit(ListChange<T>{ListChangeKind::Reset, 0, nullptr, 0});
    }

private:
    // Per-instance state shared with the source-listener lambda.
    struct SharedState {
        mutable std::shared_mutex       m;
        Comparator                      comparator;
        // length == source.size(); value = derived index of that source slot.
        std::vector<std::size_t>        source_to_derived;
        // length == derived.size(); value = source index.
        std::vector<std::size_t>        derived_to_source;
        // parallel to derived_to_source; strong refs for at() / snapshot().
        std::vector<std::shared_ptr<T>> items;
    };

    std::shared_ptr<ObservableList<T>> source_;
    std::shared_ptr<Signal>            signal_;
    std::shared_ptr<SharedState>       state_;
    Subscription                       source_sub_;

    // Rebuild all three parallel vectors from a fresh source snapshot.
    // Caller must hold the unique lock on `st.m`. `snap` is taken
    // from the source, whose lock is independent of ours.
    static void rebuild_from_snapshot_unlocked_(
        SharedState& st,
        std::vector<std::shared_ptr<T>> snap)
    {
        std::vector<std::size_t> idx(snap.size());
        for (std::size_t i = 0; i < snap.size(); ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(),
            [&](std::size_t a, std::size_t b) {
                return st.comparator(*snap[a], *snap[b]);
            });

        st.source_to_derived.assign(snap.size(), 0);
        st.derived_to_source.assign(idx.size(), 0);
        st.items.clear();
        st.items.reserve(idx.size());
        for (std::size_t d = 0; d < idx.size(); ++d) {
            const std::size_t s = idx[d];
            st.derived_to_source[d] = s;
            st.source_to_derived[s] = d;
            st.items.push_back(snap[s]);
        }
    }

    // ── Translation: one source event -> zero or more derived events ──
    static void dispatch_source_change_(SharedState& st,
                                        Signal& sig,
                                        ObservableList<T>& src,
                                        const ListChange<T>& ch) {
        switch (ch.kind) {
        case ListChangeKind::Insert:      handle_insert_(st, sig, src, ch);      return;
        case ListChangeKind::Remove:      handle_remove_(st, sig, ch);           return;
        case ListChangeKind::Replace:     handle_replace_(st, sig, src, ch);     return;
        case ListChangeKind::ItemChanged: handle_item_changed_(st, sig, src, ch); return;
        case ListChangeKind::Move:        handle_move_(st, sig, ch);             return;
        case ListChangeKind::Reset:       handle_reset_(st, sig, src);           return;
        }
    }

    /// Find the derived insertion position for a source item using a
    /// binary search that respects the stability tie-breaker
    /// (equivalent items are ordered by ascending source index).
    ///
    /// `skip_derived_idx`, when set, marks a derived slot that is
    /// considered absent — used by Replace / ItemChanged where we are
    /// searching for the NEW position of an item that currently
    /// occupies its OLD slot.
    ///
    /// Returns a position in the COMPRESSED view (d_old skipped when
    /// `skip_derived_idx` is set). Range: `[0, items.size() - (1 if
    /// skip else 0)]`. The caller interprets this as follows:
    ///
    ///   * no skip → insert at real index `lo`, shift everything >= lo
    ///     right by one (standard insertion).
    ///   * with skip:
    ///       - if `lo == *skip_derived_idx` → item stays at d_old
    ///         (same-slot event).
    ///       - otherwise → erase the slot at d_old first, THEN insert
    ///         at real index `lo` (which is already the correct index
    ///         in the post-erase coordinate system).
    static std::size_t binary_search_insert_(
        const SharedState& st, const T& needle, std::size_t needle_src_i,
        std::optional<std::size_t> skip_derived_idx = std::nullopt)
    {
        const auto& cmp   = st.comparator;
        const auto& items = st.items;
        const auto& d2s   = st.derived_to_source;

        // `real` translates from compressed (with d_old skipped) to
        // real derived-index. Only used to access items[] / d2s[]
        // during the search — the returned value stays in compressed
        // coordinates.
        auto real = [&](std::size_t compressed) -> std::size_t {
            if (skip_derived_idx && compressed >= *skip_derived_idx)
                return compressed + 1;
            return compressed;
        };
        const std::size_t n_eff = items.size()
                                  - (skip_derived_idx ? 1u : 0u);

        std::size_t lo = 0;
        std::size_t hi = n_eff;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            const std::size_t r   = real(mid);
            const auto& mid_item  = *items[r];
            if (cmp(needle, mid_item)) {
                hi = mid;
            } else if (cmp(mid_item, needle)) {
                lo = mid + 1;
            } else {
                // Equivalent keys — stability tie-breaker by source
                // index.
                if (needle_src_i < d2s[r]) hi = mid;
                else                        lo = mid + 1;
            }
        }
        return lo;
    }

    static void handle_insert_(SharedState& st, Signal& sig,
                               ObservableList<T>& src,
                               const ListChange<T>& ch) {
        std::unique_lock lk(st.m);
        const std::size_t src_idx = ch.index;

        // Shift all source indices >= src_idx by +1 (the source just
        // grew one slot before/at them).
        for (auto& s : st.derived_to_source) {
            if (s >= src_idx) ++s;
        }
        st.source_to_derived.insert(st.source_to_derived.begin()
                                    + static_cast<std::ptrdiff_t>(src_idx),
                                    0);

        // Resolve the current shared_ptr from the source. source has
        // already released its own lock by the time emit() runs.
        auto shared = src.at(src_idx);

        // Binary-search the sorted position for the new item.
        const std::size_t d_idx = binary_search_insert_(st, *shared, src_idx);

        st.derived_to_source.insert(st.derived_to_source.begin()
                                    + static_cast<std::ptrdiff_t>(d_idx),
                                    src_idx);
        st.items.insert(st.items.begin()
                        + static_cast<std::ptrdiff_t>(d_idx),
                        shared);

        // Rebuild s2d from d2s (the insert shifted all derived
        // indices >= d_idx by +1).
        renumber_s2d_(st);

        lk.unlock();
        sig.emit(ListChange<T>{ListChangeKind::Insert, d_idx, ch.item, 0});
    }

    static void handle_remove_(SharedState& st, Signal& sig,
                               const ListChange<T>& ch) {
        std::unique_lock lk(st.m);
        const std::size_t src_idx = ch.index;
        if (src_idx >= st.source_to_derived.size()) return;

        const std::size_t d_idx = st.source_to_derived[src_idx];
        auto removed = st.items[d_idx];

        st.items.erase(st.items.begin() + static_cast<std::ptrdiff_t>(d_idx));
        st.derived_to_source.erase(
            st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(d_idx));
        st.source_to_derived.erase(
            st.source_to_derived.begin() + static_cast<std::ptrdiff_t>(src_idx));

        // All source indices > src_idx shift down by 1.
        for (auto& s : st.derived_to_source) {
            if (s > src_idx) --s;
        }
        renumber_s2d_(st);

        lk.unlock();
        sig.emit(ListChange<T>{ListChangeKind::Remove, d_idx, removed.get(), 0});
    }

    static void handle_replace_(SharedState& st, Signal& sig,
                                ObservableList<T>& src,
                                const ListChange<T>& ch) {
        handle_slot_changed_(st, sig, src, ch,
                             /*new_ptr_from_src=*/true,
                             /*same_slot_kind=*/ListChangeKind::Replace,
                             /*cross_slot_use_move=*/false);
    }

    static void handle_item_changed_(SharedState& st, Signal& sig,
                                     ObservableList<T>& src,
                                     const ListChange<T>& ch) {
        handle_slot_changed_(st, sig, src, ch,
                             /*new_ptr_from_src=*/false,
                             /*same_slot_kind=*/ListChangeKind::ItemChanged,
                             /*cross_slot_use_move=*/true);
    }

    /// Shared body for Replace / ItemChanged.
    ///
    /// Both events mean "the item at source index `src_i` may now sort
    /// to a different derived position". They differ in:
    ///   * Replace replaces the shared_ptr in `items`; ItemChanged
    ///     keeps the same shared_ptr (pointed-to object mutated in
    ///     place).
    ///   * When the item moves to a different derived slot:
    ///       Replace    → Remove(d_old) + Insert(d_new)   (identity broke)
    ///       ItemChanged→ Move(d_old, d_new, item)        (same object)
    static void handle_slot_changed_(SharedState& st, Signal& sig,
                                     ObservableList<T>& src,
                                     const ListChange<T>& ch,
                                     bool new_ptr_from_src,
                                     ListChangeKind same_slot_kind,
                                     bool cross_slot_use_move) {
        std::unique_lock lk(st.m);
        const std::size_t src_idx = ch.index;
        if (src_idx >= st.source_to_derived.size()) return;

        const std::size_t d_old = st.source_to_derived[src_idx];

        // Refresh the shared_ptr for Replace (the source already
        // dropped its lock before emitting, so at() is safe).
        auto fresh = new_ptr_from_src ? src.at(src_idx) : st.items[d_old];
        if (new_ptr_from_src) st.items[d_old] = fresh;

        // Where does the (possibly mutated) item belong now? Skip the
        // old slot in the search so we measure "new position if the
        // old slot weren't there". Returned `p_small` is in the
        // compressed (d_old-skipped) view: `p_small == d_old` means
        // the item belongs right back where it was.
        const std::size_t p_small = binary_search_insert_(
            st, *fresh, src_idx, /*skip_derived_idx=*/d_old);

        if (p_small == d_old) {
            // Item stayed in place — one same-slot event.
            lk.unlock();
            sig.emit(ListChange<T>{same_slot_kind, d_old, fresh.get(), 0});
            return;
        }

        // The slot moved. Erase from d_old, then insert at p_small
        // (which already lives in the post-erase coordinate system).
        auto item = st.items[d_old];
        st.items.erase(st.items.begin() + static_cast<std::ptrdiff_t>(d_old));
        st.derived_to_source.erase(
            st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(d_old));

        const std::size_t d_insert = p_small;
        st.items.insert(st.items.begin()
                        + static_cast<std::ptrdiff_t>(d_insert),
                        item);
        st.derived_to_source.insert(
            st.derived_to_source.begin()
            + static_cast<std::ptrdiff_t>(d_insert),
            src_idx);

        renumber_s2d_(st);

        lk.unlock();
        if (cross_slot_use_move) {
            sig.emit(ListChange<T>{ListChangeKind::Move, d_insert,
                                    fresh.get(), d_old});
        } else {
            sig.emit(ListChange<T>{ListChangeKind::Remove, d_old,
                                    fresh.get(), 0});
            sig.emit(ListChange<T>{ListChangeKind::Insert, d_insert,
                                    fresh.get(), 0});
        }
    }

    /// Source Move(from, to) changes source indices but the derived
    /// ordering is a pure function of (comparator, source_index).
    /// So the derived layout is unchanged — we only need to repair
    /// the s2d / d2s index maps to match the new source positions.
    ///
    /// Subtlety: the stability tie-breaker uses source index, so a
    /// Move CAN in principle change the relative order of items
    /// that compare equivalent. That would be a re-shuffle inside an
    /// equivalence class. The choice here is to tolerate it — the common case
    /// (keys are distinct) is untouched, and "Move of an equivalent
    /// item breaks stability" is an extremely niche contract people
    /// pay for by using equivalent-but-distinguishable items.
    static void handle_move_(SharedState& st, Signal& /*sig*/,
                             const ListChange<T>& ch) {
        std::unique_lock lk(st.m);
        const std::size_t from = ch.from_index;
        const std::size_t to   = ch.index;
        if (from == to) return;
        if (from >= st.source_to_derived.size()) return;
        if (to   >= st.source_to_derived.size()) return;

        // Shift all source indices that lived in [from+1..to] left by 1
        // (if from < to) or all indices in [to..from-1] right by 1
        // (if from > to). The moved item itself gets the new value
        // `to`.
        const std::size_t moved_d = st.source_to_derived[from];

        if (from < to) {
            for (auto& s : st.derived_to_source) {
                if (s > from && s <= to) --s;
            }
        } else {
            for (auto& s : st.derived_to_source) {
                if (s >= to && s < from) ++s;
            }
        }
        st.derived_to_source[moved_d] = to;
        renumber_s2d_(st);
    }

    static void handle_reset_(SharedState& st, Signal& sig,
                              ObservableList<T>& src) {
        {
            std::unique_lock lk(st.m);
            rebuild_from_snapshot_unlocked_(st, src.snapshot());
        }
        sig.emit(ListChange<T>{ListChangeKind::Reset, 0, nullptr, 0});
    }

    /// Restore the `source_to_derived` inverse mapping from the
    /// authoritative `derived_to_source` after any mutation that
    /// shifted indices.
    static void renumber_s2d_(SharedState& st) {
        for (std::size_t d = 0; d < st.derived_to_source.size(); ++d) {
            st.source_to_derived[st.derived_to_source[d]] = d;
        }
    }
};

}  // namespace aria
