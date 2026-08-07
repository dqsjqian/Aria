#pragma once

// FilteredList<T> — a live-updating filtered view over an ObservableList<T>.
//
// Given a source list and a predicate, FilteredList exposes the subset of
// items for which the predicate returns true, along with the same
// observation surface (ListChange events, snapshot, at/size). Every change
// to the source is translated into the minimal incremental sequence of
// derived events; small source mutations **never** escalate to a full
// Reset on the derived side.
//
// Usage:
//
//     auto source = std::make_shared<ObservableList<Task>>();
//     auto active = std::make_shared<FilteredList<Task>>(
//         source,
//         [](const Task& t) { return !t.is_done(); });
//
//     auto sub = active->observe([](const ListChange<Task>& ch) {
//         // UI-side handler; receives derived-coordinate events.
//     });
//
//     source->push_back(std::make_shared<Task>("write docs"));
//     // → active sees Insert(0)
//     source->at(0)->mark_done();
//     // → active sees Remove(0)    (if Task exposes on_changed)
//
// Event-translation contract (mandatory; pinned by tests):
//
//     Source event            Derived behaviour
//     ────────────────        ──────────────────────────────────────────
//     Insert(i, x)            p(x) ? Insert(j, x) : nothing
//     Remove(i)               was-in ? Remove(j)  : nothing
//     Replace(i, x_new)       in→in  : Replace(j, x_new)
//                             in→out : Remove(j)
//                             out→in : Insert(j, x_new)
//                             out→out: nothing
//     ItemChanged(i)          in still in  : ItemChanged(j)
//                             in  → out    : Remove(j)
//                             out → in     : Insert(j, <cur>)
//                             out still out: nothing
//     Move(from, to, x)       in→in  : Move(j_from, j_to, x)
//                                      (j_from / j_to recomputed from
//                                      the post-move layout)
//                             items not in the filter produce no event
//     Reset                   always : Reset
//
// set_predicate() applies a membership diff: each source item that
// transitions in↔out produces a single Insert / Remove on the derived
// side. Items that stay in or stay out produce no event.
//
// Thread-safety: the derived list protects its own state with a
// `shared_mutex`. The listener attached to the source is invoked on
// whatever thread the source emits on (ObservableList emits AFTER
// releasing its own mutex, so the listener may call back into the
// source safely). FilteredList itself is graph-thread-agnostic;
// callers who want UI-thread delivery should put a dispatcher between
// the source emission and the FilteredList observer.
//
// Lifetime:
// - FilteredList holds a strong shared_ptr to its source, so the source is
//   guaranteed to outlive the derived view. This holds transitively when
//   sources are chained: each link keeps the one below it alive.
// - The source listener captures a weak_ptr<SharedState> and a
//   weak_ptr<Signal>; destroying the FilteredList mid-emission
//   degrades the listener to a no-op and the Subscription RAII
//   releases the connection cleanly.
//
// Chaining:
// - The source type is a template parameter defaulting to
//   `ObservableList<T>` and constrained to `ListSourceOf<Source, T>`. Any
//   type satisfying that concept works, which is what makes
//   `FilteredList -> SortedList -> PagedList` pipelines possible. Prefer the
//   `filtered()` helper at the bottom of this file so the source type does
//   not have to be spelled out.

#include "aria/inplace_function.hpp"
#include "aria/list_source.hpp"
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

template<typename T, typename Source = ObservableList<T>>
    requires ListSourceOf<Source, T>
class FilteredList
    : public detail::ListSignalMixin<FilteredList<T, Source>, T> {
    friend detail::ListSignalMixin<FilteredList<T, Source>, T>;

public:
    using value_type = T;
    /// Owning, heap-free predicate handle. Captures up to 32 bytes;
    /// larger captures are a compile-time error (by design — derived
    /// list hot paths must never allocate behind the user's back).
    using Predicate  = aria::inplace_function<bool(const T&), 32>;
    using Signal     = detail::TypedSignal<ListChange<T>>;

    FilteredList(std::shared_ptr<Source> source,
                 Predicate predicate)
        : source_(std::move(source)),
          signal_(std::make_shared<Signal>()),
          state_(std::make_shared<SharedState>())
    {
        state_->predicate = std::move(predicate);

        // Build the initial mapping from the source snapshot. Taking
        // the snapshot BEFORE subscribing means concurrent source
        // mutations that arrive mid-construction are delivered as
        // normal listener callbacks on top of the known-good baseline.
        {
            std::unique_lock lk(state_->m);
            auto snap = source_->snapshot();
            state_->source_to_derived.assign(snap.size(), std::nullopt);
            for (std::size_t i = 0; i < snap.size(); ++i) {
                if (state_->predicate(*snap[i])) {
                    state_->source_to_derived[i] = state_->derived_to_source.size();
                    state_->derived_to_source.push_back(i);
                    state_->items.push_back(snap[i]);
                }
            }
        }

        // Subscribe on the source. The lambda holds only weak_ptrs so
        // destroying the FilteredList while a notification is in
        // flight is safe — the lock() attempt returns nullptr.
        std::weak_ptr<SharedState> weak_state  = state_;
        std::weak_ptr<Signal>      weak_signal = signal_;
        std::weak_ptr<Source> weak_source{source_};
        source_sub_ = source_->observe(
            [weak_state, weak_signal, weak_source](const ListChange<T>& ch) {
                auto st  = weak_state.lock();
                auto sig = weak_signal.lock();
                auto src = weak_source.lock();
                if (!st || !sig || !src) return;
                dispatch_source_change_(*st, *sig, *src, ch);
            });
    }

    ~FilteredList() = default;

    FilteredList(const FilteredList&)            = delete;
    FilteredList& operator=(const FilteredList&) = delete;

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

    // ── Predicate replacement (incremental diff) ──────────────────────
    //
    // For each source item, compute the new membership; emit Insert /
    // Remove only for items whose membership changed. Items that stay
    // in or stay out produce no event — if the caller wanted "refresh
    // as if every in-item had changed", they should instead send a
    // proper source-side mutation or iterate ItemChanged manually.
    void set_predicate(Predicate new_predicate) {
        struct PendingEmit {
            ListChangeKind kind;
            std::size_t    derived_index;
            const T*       item_ptr;
        };
        std::vector<PendingEmit> emissions;

        {
            std::unique_lock lk(state_->m);
            state_->predicate = std::move(new_predicate);

            auto snap = source_->snapshot();

            // Build new mapping in a single pass; the OLD mapping is
            // still in `state_->source_to_derived` at this point so we
            // can detect membership transitions.
            std::vector<std::optional<std::size_t>> new_s2d(snap.size(), std::nullopt);
            std::vector<std::size_t>                new_d2s;
            std::vector<std::shared_ptr<T>>         new_items;
            new_d2s.reserve(snap.size());
            new_items.reserve(snap.size());

            // Emission indices must follow D-11 "as observed": each event's
            // index reflects the derived list as the OBSERVER sees it at the
            // moment of that emit, not the pre-change or post-change layout.
            //
            // We therefore walk the source in order and maintain
            // `observed_pos` — the index, in the observer's incrementally
            // rebuilt mirror, of the next element that survives. Items that
            // stay in advance it; a Remove leaves it alone (the mirror just
            // shrank at that spot); an Insert lands at it and advances it.
            //
            // Getting this wrong is not a cosmetic bug. The previous version
            // emitted Remove with the OLD derived index and Insert with the
            // NEW one, mixing two coordinate systems: for source [A,B,C] all
            // passing, with a new predicate that keeps only C, it emitted
            // Remove(0), Remove(1) — walking the observer's mirror
            // [A,B,C] -> [B,C] -> [B], while the real state is [C]. The
            // mirror was then permanently wrong with no event to repair it.
            // The correct stream here is Remove(0), Remove(0).
            std::size_t observed_pos = 0;

            for (std::size_t i = 0; i < snap.size(); ++i) {
                const bool was_in = (i < state_->source_to_derived.size())
                                    && state_->source_to_derived[i].has_value();
                const bool is_in  = state_->predicate(*snap[i]);

                if (is_in) {
                    new_s2d[i] = new_d2s.size();
                    new_d2s.push_back(i);
                    new_items.push_back(snap[i]);
                }

                if (was_in && !is_in) {
                    // Dropped out: the observer removes at `observed_pos`,
                    // and everything after it shifts down — so
                    // `observed_pos` stays put for the next candidate.
                    emissions.push_back({
                        ListChangeKind::Remove,
                        observed_pos,
                        snap[i].get()});
                } else if (!was_in && is_in) {
                    // Newly admitted: lands at `observed_pos` in the mirror.
                    emissions.push_back({
                        ListChangeKind::Insert,
                        observed_pos,
                        snap[i].get()});
                    ++observed_pos;
                } else if (was_in && is_in) {
                    // Unchanged member: no event, but it occupies a slot in
                    // the observer's mirror.
                    ++observed_pos;
                }
                // (!was_in && !is_in): absent before and after — no slot.
            }

            state_->source_to_derived = std::move(new_s2d);
            state_->derived_to_source = std::move(new_d2s);
            state_->items             = std::move(new_items);
        }

        // Emit outside the lock. Observers that call back into at() /
        // snapshot() see the new state, matching ObservableList's own
        // post-mutation emission contract.
        for (const auto& e : emissions) {
            signal_->emit(ListChange<T>{e.kind, e.derived_index, e.item_ptr, 0});
        }
    }

private:
    // Per-instance state shared with the source-listener lambda. Held
    // as shared_ptr so the listener never dereferences a dangling
    // control block.
    struct SharedState {
        mutable std::shared_mutex                 m;
        Predicate                                 predicate;
        // length == source.size(); nullopt means "not in derived".
        std::vector<std::optional<std::size_t>>   source_to_derived;
        // length == derived.size(); value is the source index.
        std::vector<std::size_t>                  derived_to_source;
        // parallel to derived_to_source; strong refs for at() / snapshot().
        std::vector<std::shared_ptr<T>>           items;
    };

    std::shared_ptr<Source> source_;
    std::shared_ptr<Signal>            signal_;
    std::shared_ptr<SharedState>       state_;
    Subscription                       source_sub_;

    // ── Translation: one source event -> zero or one derived events ───
    static void dispatch_source_change_(SharedState& st,
                                        Signal& sig,
                                        Source& src,
                                        const ListChange<T>& ch) {
        switch (ch.kind) {
        case ListChangeKind::Insert:      handle_insert_(st, sig, src, ch);      return;
        case ListChangeKind::Remove:      handle_remove_(st, sig, ch);           return;
        case ListChangeKind::Replace:     handle_replace_(st, sig, src, ch);     return;
        case ListChangeKind::ItemChanged: handle_item_changed_(st, sig, src, ch); return;
        case ListChangeKind::Move:        handle_move_(st, sig, ch);             return;
        case ListChangeKind::Reset:       handle_reset_(st, sig);                return;
        }
    }

    static void handle_insert_(SharedState& st, Signal& sig,
                               Source& src,
                               const ListChange<T>& ch) {
        std::unique_lock lk(st.m);
        const std::size_t src_idx = ch.index;

        // All existing d2s entries >= src_idx shifted by +1 in the
        // source (the source just inserted one slot before/at them).
        for (auto& s : st.derived_to_source) {
            if (s >= src_idx) ++s;
        }
        // Insert the new "not yet classified" source slot.
        st.source_to_derived.insert(st.source_to_derived.begin() + static_cast<std::ptrdiff_t>(src_idx),
                                    std::nullopt);

        const bool is_in = st.predicate(*ch.item);
        if (!is_in) {
            renumber_s2d_(st);
            return;
        }

        // Compute derived insertion index: number of in-filter source
        // slots strictly before src_idx.
        std::size_t d_idx = 0;
        for (std::size_t i = 0; i < src_idx; ++i) {
            if (st.source_to_derived[i].has_value()) ++d_idx;
        }

        st.derived_to_source.insert(st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(d_idx),
                                    src_idx);

        // Resolve the current shared_ptr from the source by index.
        // The source has already released its own lock by the time
        // emit() runs (see ObservableList::push_back), so this at()
        // is deadlock-free.
        auto shared = src.at(src_idx);
        st.items.insert(st.items.begin() + static_cast<std::ptrdiff_t>(d_idx), std::move(shared));

        st.source_to_derived[src_idx] = d_idx;
        renumber_s2d_(st);

        lk.unlock();
        sig.emit(ListChange<T>{ListChangeKind::Insert, d_idx, ch.item, 0});
    }

    static void handle_remove_(SharedState& st, Signal& sig,
                               const ListChange<T>& ch) {
        std::unique_lock lk(st.m);
        const std::size_t src_idx = ch.index;

        if (src_idx >= st.source_to_derived.size()) return;   // defensive

        std::optional<std::size_t> maybe_d = st.source_to_derived[src_idx];
        st.source_to_derived.erase(st.source_to_derived.begin() + static_cast<std::ptrdiff_t>(src_idx));

        // All d2s entries > src_idx shifted by -1 (source collapsed).
        for (auto& s : st.derived_to_source) {
            if (s > src_idx) --s;
        }

        if (maybe_d) {
            const std::size_t d_idx = *maybe_d;
            auto removed = st.items[d_idx];   // keep alive past erase
            st.items.erase(st.items.begin() + static_cast<std::ptrdiff_t>(d_idx));
            st.derived_to_source.erase(st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(d_idx));
            renumber_s2d_(st);
            lk.unlock();
            sig.emit(ListChange<T>{ListChangeKind::Remove, d_idx,
                                   removed.get(), 0});
            return;
        }

        renumber_s2d_(st);
    }

    /// Both `Replace` and `ItemChanged` from the source go through the
    /// same four-quadrant `(was_in, is_in)` membership transition. The
    /// only differences:
    ///   * Replace also swaps the underlying item value (refresh items[]
    ///     in the in→in case);
    ///   * the in→in case emits a different ListChangeKind on the
    ///     derived side.
    /// `kind_for_in_in` selects the right derived event for the in→in
    /// quadrant; `refresh_value` selects whether to copy a fresh
    /// shared_ptr from the source. Out→in still always emits Insert,
    /// in→out still always emits Remove.
    static void handle_membership_transition_(SharedState& st, Signal& sig,
                                              Source& src,
                                              const ListChange<T>& ch,
                                              ListChangeKind kind_for_in_in,
                                              bool refresh_value) {
        std::unique_lock lk(st.m);
        const std::size_t src_idx = ch.index;

        if (src_idx >= st.source_to_derived.size()) return;

        const bool was_in = st.source_to_derived[src_idx].has_value();
        const bool is_in  = st.predicate(*ch.item);

        if (!was_in && !is_in) return;

        if (was_in && is_in) {
            const std::size_t d_idx = *st.source_to_derived[src_idx];
            if (refresh_value) {
                st.items[d_idx] = src.at(src_idx);
            }
            lk.unlock();
            sig.emit(ListChange<T>{kind_for_in_in, d_idx, ch.item, 0});
            return;
        }

        if (was_in && !is_in) {
            const std::size_t d_idx = *st.source_to_derived[src_idx];
            auto removed = st.items[d_idx];
            st.items.erase(st.items.begin() + static_cast<std::ptrdiff_t>(d_idx));
            st.derived_to_source.erase(st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(d_idx));
            st.source_to_derived[src_idx] = std::nullopt;
            renumber_s2d_(st);
            lk.unlock();
            sig.emit(ListChange<T>{ListChangeKind::Remove, d_idx,
                                   removed.get(), 0});
            return;
        }

        // !was_in && is_in: item became visible.
        std::size_t d_idx = 0;
        for (std::size_t i = 0; i < src_idx; ++i) {
            if (st.source_to_derived[i].has_value()) ++d_idx;
        }
        st.derived_to_source.insert(st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(d_idx),
                                    src_idx);
        st.items.insert(st.items.begin() + static_cast<std::ptrdiff_t>(d_idx), src.at(src_idx));
        st.source_to_derived[src_idx] = d_idx;
        renumber_s2d_(st);
        lk.unlock();
        sig.emit(ListChange<T>{ListChangeKind::Insert, d_idx, ch.item, 0});
    }

    static void handle_replace_(SharedState& st, Signal& sig,
                                Source& src,
                                const ListChange<T>& ch) {
        handle_membership_transition_(st, sig, src, ch,
                                      ListChangeKind::Replace,
                                      /*refresh_value=*/true);
    }

    static void handle_item_changed_(SharedState& st, Signal& sig,
                                     Source& src,
                                     const ListChange<T>& ch) {
        handle_membership_transition_(st, sig, src, ch,
                                      ListChangeKind::ItemChanged,
                                      /*refresh_value=*/false);
    }

    static void handle_move_(SharedState& st, Signal& sig,
                             const ListChange<T>& ch) {
        std::unique_lock lk(st.m);
        const std::size_t from = ch.from_index;
        const std::size_t to   = ch.index;

        if (from == to) return;
        if (from >= st.source_to_derived.size() ||
            to   >= st.source_to_derived.size()) {
            return;   // defensive; source mutation out of step
        }

        // Derived index of the moved item BEFORE shuffle (if any).
        const std::optional<std::size_t> old_d = st.source_to_derived[from];

        // Shuffle source_to_derived to mirror the source's own move.
        auto moved_slot = st.source_to_derived[from];
        st.source_to_derived.erase(st.source_to_derived.begin() + static_cast<std::ptrdiff_t>(from));
        st.source_to_derived.insert(st.source_to_derived.begin() + static_cast<std::ptrdiff_t>(to),
                                    moved_slot);

        // If the moved item was in the filter, its derived position
        // needs updating too. Compute the new derived index of the
        // item now at source `to`.
        if (old_d) {
            // Determine new derived index by counting in-filter slots
            // strictly before `to` in the new source layout.
            std::size_t new_d = 0;
            for (std::size_t i = 0; i < to; ++i) {
                if (st.source_to_derived[i].has_value()) ++new_d;
            }

            // Physically move the item within derived_to_source and
            // items to reflect the new position.
            if (new_d != *old_d) {
                auto moved_src = st.derived_to_source[*old_d];
                auto moved_item = st.items[*old_d];
                st.derived_to_source.erase(st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(*old_d));
                st.items.erase(st.items.begin() + static_cast<std::ptrdiff_t>(*old_d));
                st.derived_to_source.insert(
                    st.derived_to_source.begin() + static_cast<std::ptrdiff_t>(new_d), moved_src);
                st.items.insert(st.items.begin() + static_cast<std::ptrdiff_t>(new_d), std::move(moved_item));
            }

            // Now renumber s2d to reflect both the source shuffle and
            // the derived reordering we just did.
            renumber_s2d_(st);

            if (new_d != *old_d) {
                lk.unlock();
                sig.emit(ListChange<T>{ListChangeKind::Move, new_d,
                                       ch.item, *old_d});
            }
            return;
        }

        // Moved item was not in the filter — the only change is which
        // source indices each d2s entry refers to.
        renumber_d2s_after_source_move_(st, from, to);
        renumber_s2d_(st);
    }

    static void handle_reset_(SharedState& st, Signal& sig) {
        std::unique_lock lk(st.m);
        st.source_to_derived.clear();
        st.derived_to_source.clear();
        st.items.clear();
        lk.unlock();
        sig.emit(ListChange<T>{ListChangeKind::Reset, 0, nullptr, 0});
    }

    // Walk source_to_derived and assign successive derived indices to
    // every non-nullopt slot. O(source.size()). Call after any
    // structural mutation.
    static void renumber_s2d_(SharedState& st) {
        std::size_t d = 0;
        for (auto& slot : st.source_to_derived) {
            if (slot.has_value()) slot = d++;
        }
    }

    // When a filtered-out item moves within the source, we still need
    // to update the source indices stored in derived_to_source so that
    // they refer to the right slots. This is the straightforward
    // translation: values > from and <= to (moving backward) or
    // >= to and < from (moving forward) get shifted by one.
    static void renumber_d2s_after_source_move_(SharedState& st,
                                                std::size_t from,
                                                std::size_t to) {
        if (from < to) {
            for (auto& s : st.derived_to_source) {
                if (s > from && s <= to) --s;
                // NOTE: the moved item itself was NOT in derived,
                // so we never encounter `s == from`.
            }
        } else {
            for (auto& s : st.derived_to_source) {
                if (s >= to && s < from) ++s;
            }
        }
    }
};

// ---------------------------------------------------------------------------
//  Factory helper — deduces the source type so pipelines stay readable.
//
//  Without it, chaining forces the caller to spell out every layer:
//
//    auto f = std::make_shared<FilteredList<Task>>(src, pred);
//    auto s = std::make_shared<SortedList<Task, FilteredList<Task>>>(f, cmp);
//
//  With it:
//
//    auto f = aria::filtered(src, pred);
//    auto s = aria::sorted(f, cmp);
//
//  Returns shared_ptr because every derived list takes its source as one, so
//  the result is immediately usable as the next link in the chain.
// ---------------------------------------------------------------------------
template<typename Source,
         typename Predicate,
         typename T = list_source_value_t<Source>>
    requires ListSourceOf<Source, T>
[[nodiscard]] std::shared_ptr<FilteredList<T, Source>>
filtered(std::shared_ptr<Source> source, Predicate predicate) {
    return std::make_shared<FilteredList<T, Source>>(
        std::move(source),
        typename FilteredList<T, Source>::Predicate{std::move(predicate)});
}

}  // namespace aria
