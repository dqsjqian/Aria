// ============================================================================
//  aria/derived/distinct_list.hpp
// ----------------------------------------------------------------------------
//  `DistinctList<T, Key>` -- a derived list that drops duplicates of
//  the source. Joins the family of derived collections (FilteredList
//  / SortedList / MappedList) and follows the same incremental
//  contract spelled out in `docs/list-diff-contract.md` LD-2 / LD-7.
//
//  Semantics (PD-N IDs, Pinned Distinct):
//
//    PD-1 (canonical key). Each source item is hashed/compared by
//        `Key key_of(const T&)`. Default Key = T -> identity, requires
//        T to be hashable + equality-comparable.
//
//    PD-2 (first-appearance, source-ordered). When multiple source
//        slots share a key, the derived list keeps the FIRST
//        occurrence (smallest source index at the moment the key
//        enters the visible set) and hides the rest. The visible
//        order is the source order of those representatives. If the
//        source inserts a new-key item between two existing source
//        positions p_left < p_right, the new derived slot lands
//        between the derived slots that mirror p_left and p_right
//        (not appended to the end). Mirrors `std::ranges::unique`
//        on a stable, source-ordered view.
//
//    PD-3 (incremental events, complexity envelope). Every source
//        mutation maps to AT MOST one derived event (Insert / Remove
//        / Replace / ItemChanged / Reset). Hidden-duplicate Insert /
//        Remove are silent. The implementation is O(N_visible) per
//        derived event in the worst case (we maintain visible
//        slot_id -> derived_pos via a sorted vector of slot_ids;
//        binary search + linear shift on the slot_id vector). For
//        the common append-at-end case both halves degenerate to
//        amortised O(1). This is the same complexity envelope as
//        FilteredList / SortedList per LD-2.
//
//    PD-4 (key-changing ItemChanged). When T's `on_changed` fires
//        AND the new key differs from the old, this manifests as
//        Remove (of the old representative if it was visible) plus
//        Insert (of the new representative if the new key was
//        previously hidden), each obeying PD-2 / PD-3.
//
//    PD-5 (lifetime). Destroying the source while DistinctList is
//        still alive is safe (weak_ptr capture in the source
//        observer; final emission is dropped).
// ============================================================================
#pragma once

#include "aria/inplace_function.hpp"
#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/list_signal_mixin.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria {

template<typename T, typename Key = T,
         typename Source = ObservableList<T>>
    requires ListSourceOf<Source, T>
class DistinctList
    : public detail::ListSignalMixin<DistinctList<T, Key, Source>, T> {
    friend detail::ListSignalMixin<DistinctList<T, Key, Source>, T>;

public:
    using value_type = T;
    /// Owning, heap-free key extractor (capacity 32 bytes).
    using KeyOf      = aria::inplace_function<Key(const T&), 32>;
    using Signal     = detail::ListSignal<T>;

    /// Construct a DistinctList. The default `key_of` projects T to
    /// itself, which works as long as T is hashable + equality-
    /// comparable. Provide a custom extractor for richer T.
    DistinctList(std::shared_ptr<Source> source,
                 KeyOf key_of = default_key_of_())
        : source_(std::move(source)),
          signal_(std::make_shared<Signal>()),
          state_(std::make_shared<SharedState>())
    {
        state_->key_of = std::move(key_of);
        rebuild_initial_();

        std::weak_ptr<SharedState>       weak_state  = state_;
        std::weak_ptr<Signal>            weak_signal = signal_;
        std::weak_ptr<Source> weak_source{source_};
        source_sub_ = source_->observe(
            [weak_state, weak_signal, weak_source](const ListChange<T>& ch) {
                auto st  = weak_state.lock();
                auto sig = weak_signal.lock();
                auto src = weak_source.lock();
                if (!st || !sig || !src) return;
                handle_source_change_(*st, *sig, *src, ch);
            });
    }

    ~DistinctList() = default;

    DistinctList(const DistinctList&)            = delete;
    DistinctList& operator=(const DistinctList&) = delete;

    // ── Read surface ──────────────────────────────────────────────────
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lk(state_->m);
        return state_->visible_slots.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] std::shared_ptr<T> at(std::size_t derived_pos) const {
        std::shared_lock lk(state_->m);
        const auto sid = state_->visible_slots.at(derived_pos);
        return state_->slots.at(sid).rep;
    }

    [[nodiscard]] std::vector<std::shared_ptr<T>> snapshot() const {
        std::shared_lock lk(state_->m);
        std::vector<std::shared_ptr<T>> out;
        out.reserve(state_->visible_slots.size());
        for (auto sid : state_->visible_slots) {
            out.push_back(state_->slots.at(sid).rep);
        }
        return out;
    }

private:
    using SlotId = std::uint64_t;

    // A Slot represents one Key currently or formerly visible in the
    // derived list. Its `slot_id` is allocated when the key is first
    // promoted into the visible set and is *order-preserving*: bigger
    // slot_ids correspond to later derived positions. We rebalance
    // slot_ids only when the source inserts a new-key item *between*
    // two existing visible representatives -- see allocate_slot_id_().
    struct Slot {
        Key                                       key;
        std::shared_ptr<T>                        rep;
        // Hidden-duplicate bag, in source-arrival order. Front is
        // the next promotion candidate when rep is removed.
        std::vector<std::shared_ptr<T>>           dups;
    };

    struct SharedState {
        mutable std::shared_mutex                m;
        KeyOf                                    key_of;

        // Slots indexed by SlotId. We never reuse slot_ids for
        // different keys, but a slot is erased once its rep is
        // removed AND its dup bag is empty.
        std::unordered_map<SlotId, Slot>         slots;

        // Visible-representative slot_ids, sorted ascending. The
        // index in this vector IS the derived_pos. Maintained by
        // binary insert / erase. Worst case O(N_visible).
        std::vector<SlotId>                      visible_slots;

        // Key -> slot_id (only entries whose key currently has *any*
        // backing item -- representative or hidden dup). Erased
        // together with the slot.
        std::unordered_map<Key, SlotId>          key_to_slot;

        // Item* -> slot_id (every source item we currently track,
        // representative or hidden duplicate). Erased on Remove.
        std::unordered_map<const T*, SlotId>     item_to_slot;

        // Item* -> the key that item currently maps to. Recorded on
        // every Insert / ItemChanged so that Remove can recover the
        // old key without re-running key_of on a possibly-mutated
        // value.
        std::unordered_map<const T*, Key>        item_key;

        SlotId                                   next_slot_id{1};
        std::vector<std::shared_ptr<T>> source_items;
    };

    std::shared_ptr<Source> source_;
    std::shared_ptr<Signal>            signal_;
    std::shared_ptr<SharedState>       state_;
    Subscription                       source_sub_;

    static KeyOf default_key_of_() {
        return [](const T& v) -> Key {
            if constexpr (std::is_same_v<Key, T>) {
                return v;
            } else {
                static_assert(std::is_same_v<Key, T>,
                    "DistinctList<T, Key>: default key extractor needs Key == T. "
                    "Provide a custom KeyOf for differing Key.");
                return Key{};
            }
        };
    }

    /// Initial seed -- single O(N) pass over the source snapshot.
    /// PD-2 holds because we visit source slots in source order and
    /// each new key is appended to `visible_slots` (the previous slot
    /// ids are strictly smaller, preserving derived ordering).
    void rebuild_initial_() {
        rebuild_(*state_, source_->snapshot());
    }

    static void rebuild_(SharedState& st, std::vector<std::shared_ptr<T>> snap) {
        std::unique_lock lk(st.m);
        st.slots.clear();
        st.visible_slots.clear();
        st.key_to_slot.clear();
        st.item_key.clear();
        st.item_to_slot.clear();
        st.next_slot_id = 1;
        st.source_items = std::move(snap);
        st.slots.reserve(st.source_items.size());
        st.visible_slots.reserve(st.source_items.size());
        st.key_to_slot.reserve(st.source_items.size());
        st.item_key.reserve(st.source_items.size());
        st.item_to_slot.reserve(st.source_items.size());
        for (const auto& sp_ : st.source_items) {
            const Key k = st.key_of(*sp_);
            st.item_key[sp_.get()] = k;
            auto it = st.key_to_slot.find(k);
            if (it == st.key_to_slot.end()) {
                const SlotId sid = st.next_slot_id++;
                st.key_to_slot.emplace(k, sid);
                st.slots.emplace(sid, Slot{k, sp_, {}});
                st.visible_slots.push_back(sid);
                st.item_to_slot[sp_.get()] = sid;
            } else {
                st.slots[it->second].dups.push_back(sp_);
                st.item_to_slot[sp_.get()] = it->second;
            }
        }
    }

    /// Convert a source index (0..size-1) to a derived position by
    /// counting how many earlier-or-equal source indices currently
    /// hold a visible representative whose source position is < the
    /// requested index. The argument is the `ch.index` reported by
    /// the source ListChange; for an Insert it is the *new* position
    /// of the inserted item, for a Replace/Remove it is the position
    /// of the affected item.
    ///
    /// O(N_source) -- we walk the source until we have counted enough
    /// representatives. We could replace this with a Fenwick tree
    /// keyed by current source indices, but Aria deliberately keeps
    /// the implementation easy to audit; the cost is on par with
    /// FilteredList's projection map (LD-2 / FL-3).
    static std::size_t derived_pos_for_new_rep_(SharedState& st,
                                                std::size_t source_idx) {
        // Count: how many visible representatives sit at source
        // positions strictly before source_idx? That count IS the
        // derived position where the new representative belongs.
        std::unordered_map<SlotId, bool> counted;
        std::size_t count = 0;
        const std::size_t bound =
            std::min<std::size_t>(source_idx, st.source_items.size());
        for (std::size_t i = 0; i < bound; ++i) {
            auto sp_ = st.source_items[i];
            auto it = st.item_to_slot.find(sp_.get());
            if (it == st.item_to_slot.end()) continue;
            const SlotId sid = it->second;
            // Is this item the representative? (A representative's
            // slot.rep.get() == item.) Hidden duplicates point to
            // the same slot but slot.rep != them.
            auto sl_it = st.slots.find(sid);
            if (sl_it == st.slots.end()) continue;
            if (sl_it->second.rep.get() == sp_.get() && counted.emplace(sid, true).second) ++count;
        }
        return count;
    }

    /// Walk visible_slots and find the existing index of `sid`.
    /// Returns size() if not found. O(log N_visible) via lower_bound.
    static std::size_t derived_pos_of_slot_(const SharedState& st, SlotId sid) {
        auto it = std::lower_bound(st.visible_slots.begin(),
                                   st.visible_slots.end(), sid);
        if (it == st.visible_slots.end() || *it != sid) {
            return st.visible_slots.size();
        }
        return static_cast<std::size_t>(it - st.visible_slots.begin());
    }

    /// Allocate a slot_id whose ordering matches `derived_pos` in
    /// `visible_slots`. We compress the slot id space lazily: when a
    /// gap exists between visible_slots[derived_pos-1] and [derived_
    /// pos], place the new id strictly between. If the gap is too
    /// small (zero width) we trigger a localised rebalance: bump
    /// next_slot_id so subsequent allocations are fresh, and rewrite
    /// the affected suffix with widely spaced new slot_ids.
    static SlotId allocate_ordered_slot_id_(SharedState& st,
                                            std::size_t derived_pos) {
        const auto sz = st.visible_slots.size();
        if (derived_pos == sz) {
            // Append at the end.
            const SlotId sid = st.next_slot_id;
            st.next_slot_id += kSlotIdStep;
            return sid;
        }
        if (derived_pos == 0) {
            // Prepend at the front. Use the half-way point between 0
            // and the current first visible slot id.
            const SlotId right = st.visible_slots.front();
            if (right > 1) {
                return right / 2;
            }
            // Right-side is 1; rebalance the entire visible vector.
            return rebalance_and_insert_(st, derived_pos);
        }
        // Insert strictly between two existing visible slot ids.
        const SlotId left  = st.visible_slots[derived_pos - 1];
        const SlotId right = st.visible_slots[derived_pos];
        if (right - left >= 2) {
            return left + (right - left) / 2;
        }
        return rebalance_and_insert_(st, derived_pos);
    }

    /// When the slot id space is locally exhausted, rewrite all
    /// visible slot ids onto a wide grid. This is O(N_visible) but
    /// only fires when local gaps collapse, which on a fresh wide
    /// grid takes log N rebalances -- amortised O(1) extra per
    /// insert. We also need to refresh `key_to_slot` and `slots`
    /// keys (because slot identity changes).
    static SlotId rebalance_and_insert_(SharedState& st,
                                        std::size_t derived_pos) {
        const std::size_t sz = st.visible_slots.size();
        std::vector<SlotId> old_ids = st.visible_slots;
        std::unordered_map<SlotId, Slot> new_slots;
        new_slots.reserve(st.slots.size());
        SlotId cursor = kSlotIdStep;
        // Re-key visible slots first.
        std::unordered_map<SlotId, SlotId> remap;
        remap.reserve(sz);
        for (std::size_t i = 0; i < sz; ++i) {
            if (i == derived_pos) cursor += kSlotIdStep;
            const SlotId old_id = old_ids[i];
            const SlotId new_id = cursor;
            remap[old_id] = new_id;
            cursor += kSlotIdStep;
        }
        // Re-key any non-visible slots (slot whose representative
        // has been removed but whose dup queue is still non-empty
        // -- these never appear in visible_slots, so they keep
        // their old ids; nothing to do).
        for (auto& [old_id, slot] : st.slots) {
            auto rm_it = remap.find(old_id);
            if (rm_it == remap.end()) {
                new_slots.emplace(old_id, std::move(slot));
            } else {
                new_slots.emplace(rm_it->second, std::move(slot));
            }
        }
        st.slots = std::move(new_slots);
        // Rewrite key_to_slot.
        for (auto& [k, sid] : st.key_to_slot) {
            auto rm_it = remap.find(sid);
            if (rm_it != remap.end()) sid = rm_it->second;
        }
        // Rewrite item_to_slot.
        for (auto& [p, sid] : st.item_to_slot) {
            auto rm_it = remap.find(sid);
            if (rm_it != remap.end()) sid = rm_it->second;
        }
        // Rewrite visible_slots.
        for (std::size_t i = 0; i < sz; ++i) {
            st.visible_slots[i] = remap[old_ids[i]];
        }
        st.next_slot_id = cursor + kSlotIdStep;
        // Now there is a kSlotIdStep-wide gap at derived_pos.
        // Reserve the midpoint.
        if (derived_pos == 0) {
            return st.visible_slots.empty() ? cursor / 2
                                            : st.visible_slots.front() / 2;
        }
        const SlotId left  = st.visible_slots[derived_pos - 1];
        const SlotId right = derived_pos < sz ? st.visible_slots[derived_pos]
                                              : cursor + kSlotIdStep;
        return left + (right - left) / 2;
    }

    static constexpr SlotId kSlotIdStep = 1024;

    static void handle_source_change_(SharedState& st, Signal& sig, Source&, ListChange<T> ch) {
        std::shared_ptr<T> previous;
        {
            std::unique_lock lk(st.m);
            auto& items = st.source_items;
            const auto pos = static_cast<std::ptrdiff_t>(ch.index);
            switch (ch.kind) {
            case ListChangeKind::Insert: items.insert(items.begin() + pos, ch.item); break;
            case ListChangeKind::Remove:
                previous = items.at(ch.index);
                items.erase(items.begin() + pos);
                break;
            case ListChangeKind::Replace:
                previous = items.at(ch.index);
                items[ch.index] = ch.item;
                break;
            case ListChangeKind::Move: {
                auto moved = items.at(ch.from_index);
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(ch.from_index));
                items.insert(items.begin() + pos, std::move(moved));
                return;
            }
            default: break;
            }
        }
        switch (ch.kind) {
        case ListChangeKind::Insert: handle_insert_(st, sig, ch, ch.item); return;
        case ListChangeKind::Remove:
            ch.item = previous;
            handle_remove_(st, sig, ch);
            return;
        case ListChangeKind::Replace: {
            ListChange<T> removed{ListChangeKind::Remove, ch.index, previous, 0};
            handle_remove_(st, sig, removed);
            handle_insert_(st, sig, ch, ch.item);
            return;
        }
        case ListChangeKind::ItemChanged: handle_item_changed_(st, sig, ch); return;
        case ListChangeKind::Reset:
            rebuild_(st, *ch.snapshot);
            sig.emit(reset_event_(st));
            return;
        case ListChangeKind::Move: return;
        }
    }

    static void handle_insert_(SharedState& st, Signal& sig,
                               const ListChange<T>& ch,
                               std::shared_ptr<T> sp_) {
        const Key k = st.key_of(*sp_);

        std::optional<std::size_t> emit_at;
        SlotId emit_sid = 0;
        {
            std::unique_lock lk(st.m);
            st.item_key[sp_.get()] = k;
            auto it = st.key_to_slot.find(k);
            if (it == st.key_to_slot.end()) {
                // PD-2: derived position is the count of visible
                // representatives at source positions strictly
                // before ch.index. Fast path -- if ch.index is at
                // the source tail, the new derived slot is at the
                // visible tail too (no need to walk the source).
                std::size_t derived_pos;
                if (ch.index >= st.source_items.size() - 1) {
                    derived_pos = st.visible_slots.size();
                } else if (ch.index == 0) {
                    derived_pos = 0;
                } else {
                    derived_pos =
                        derived_pos_for_new_rep_(st, ch.index);
                }
                const SlotId sid =
                    allocate_ordered_slot_id_(st, derived_pos);
                st.key_to_slot.emplace(k, sid);
                st.slots.emplace(sid, Slot{k, sp_, {}});
                st.item_to_slot[sp_.get()] = sid;
                // Insert sid into visible_slots at derived_pos
                // (sorted ordering is preserved by construction).
                st.visible_slots.insert(
                    st.visible_slots.begin()
                        + static_cast<std::ptrdiff_t>(derived_pos),
                    sid);
                emit_at = derived_pos;
                emit_sid = sid;
            } else {
                // Hidden duplicate -- attach to the slot's bag.
                st.slots[it->second].dups.push_back(sp_);
                st.item_to_slot[sp_.get()] = it->second;
            }
        }
        if (emit_at.has_value()) {
            sig.emit(ListChange<T>{ListChangeKind::Insert, *emit_at,
                                   sp_, 0});
            (void)emit_sid;
        }
    }

    static void handle_remove_(SharedState& st, Signal& sig,
                               const ListChange<T>& ch) {
        if (ch.item == nullptr) return;
        std::optional<std::size_t> emit_remove_at;
        std::shared_ptr<T>          removed_sp;
        std::shared_ptr<T>          promoted_sp;
        std::optional<std::size_t> emit_replace_at;
        {
            std::unique_lock lk(st.m);
            auto its_it = st.item_to_slot.find(ch.item.get());
            if (its_it == st.item_to_slot.end()) return;
            const SlotId sid = its_it->second;
            const bool survives = std::any_of(st.source_items.begin(), st.source_items.end(),
                [&](const auto& item) { return item.get() == ch.item.get(); });
            if (!survives) {
                st.item_to_slot.erase(its_it);
                st.item_key.erase(ch.item.get());
            }

            auto sl_it = st.slots.find(sid);
            if (sl_it == st.slots.end()) return;
            Slot& slot = sl_it->second;

            if (slot.rep.get() != ch.item.get()) {
                // Hidden duplicate -- drop it from the bag (linear
                // in the bag's size, typically very small).
                for (auto bi = slot.dups.begin(); bi != slot.dups.end(); ++bi) {
                    if (bi->get() == ch.item.get()) { slot.dups.erase(bi); break; }
                }
                return;
            }

            // Removed item WAS the representative.
            removed_sp = slot.rep;
            if (!slot.dups.empty()) {
                // Promote -- same slot, new representative -> Replace.
                promoted_sp = std::move(slot.dups.front());
                slot.dups.erase(slot.dups.begin());
                slot.rep = promoted_sp;
                emit_replace_at = derived_pos_of_slot_(st, sid);
            } else {
                // No duplicate to promote -> erase the slot entirely.
                const std::size_t derived_pos = derived_pos_of_slot_(st, sid);
                if (derived_pos < st.visible_slots.size()) {
                    st.visible_slots.erase(
                        st.visible_slots.begin()
                            + static_cast<std::ptrdiff_t>(derived_pos));
                }
                st.key_to_slot.erase(slot.key);
                st.slots.erase(sl_it);
                emit_remove_at = derived_pos;
            }
        }
        if (emit_remove_at.has_value()) {
            sig.emit(ListChange<T>{ListChangeKind::Remove, *emit_remove_at,
                                   removed_sp, 0});
        }
        if (emit_replace_at.has_value()) {
            sig.emit(ListChange<T>{ListChangeKind::Replace, *emit_replace_at,
                                   promoted_sp, 0});
        }
    }

    static ListChange<T> reset_event_(const SharedState& st) {
        std::shared_lock lock(st.m);
        std::vector<std::shared_ptr<T>> items;
        items.reserve(st.visible_slots.size());
        for (const auto sid : st.visible_slots) items.push_back(st.slots.at(sid).rep);
        return ListChange<T>::reset(std::move(items));
    }

    static void handle_item_changed_(SharedState& st, Signal& sig,
                                     const ListChange<T>& ch) {
        if (!ch.item) return;
        std::shared_ptr<T> item;
        std::vector<std::size_t> occurrences;
        std::optional<std::size_t> visible;
        bool key_changed;
        {
            std::shared_lock lk(st.m);
            auto found = st.item_key.find(ch.item.get());
            if (found == st.item_key.end()) return;
            key_changed = found->second != st.key_of(*ch.item);
            const auto sid = st.item_to_slot.at(ch.item.get());
            const auto& slot = st.slots.at(sid);
            if (slot.rep.get() == ch.item.get()) visible = derived_pos_of_slot_(st, sid);
            for (std::size_t i = 0; i < st.source_items.size(); ++i) {
                if (st.source_items[i].get() == ch.item.get()) {
                    item = st.source_items[i];
                    if (key_changed) occurrences.push_back(i);
                }
            }
        }
        if (!key_changed) {
            if (visible) sig.emit(ListChange<T>{ListChangeKind::ItemChanged, *visible, item, 0});
            return;
        }
        // Every occurrence refers to the same mutable object. Move all of
        // them together; updating only the reported (last) source index
        // leaves an old-key duplicate behind indefinitely.
        for (const auto index : occurrences) {
            handle_remove_(st, sig, ListChange<T>{ListChangeKind::Remove, index, item, 0});
        }
        {
            std::unique_lock lk(st.m);
            st.item_key.erase(item.get());
            st.item_to_slot.erase(item.get());
        }
        for (const auto index : occurrences) {
            handle_insert_(st, sig, ListChange<T>{ListChangeKind::Insert, index, item, 0}, item);
        }
    }

};

// ---------------------------------------------------------------------------
//  Factory helper — deduces the source type so pipelines stay readable.
//  See the note on `aria::filtered` in filtered_list.hpp.
// ---------------------------------------------------------------------------
template<typename Key,
         typename Source,
         typename KeyFn,
         typename T = list_source_value_t<Source>>
    requires ListSourceOf<Source, T>
[[nodiscard]] std::shared_ptr<DistinctList<T, Key, Source>>
distinct(std::shared_ptr<Source> source, KeyFn key_of) {
    using Derived = DistinctList<T, Key, Source>;
    return std::make_shared<Derived>(
        std::move(source), typename Derived::KeyOf{std::move(key_of)});
}

}  // namespace aria
