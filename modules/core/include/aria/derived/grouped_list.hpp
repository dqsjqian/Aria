// ============================================================================
//  aria/derived/grouped_list.hpp
// ----------------------------------------------------------------------------
//  `GroupedList<T, Key>` -- a derived list whose elements are
//  `Group<T, Key>` instances, each bundling a key + an
//  `ObservableList<T>` of the source items that share that key.
//  Joins the family of derived collections (`FilteredList` /
//  `SortedList` / `MappedList` / `DistinctList` / `PagedList`).
//
//  Semantics (PGR-N IDs, "Pinned GRoup"):
//
//    PGR-1 (canonical key). Each source item is mapped via
//        `Key key_of(const T&)`. Default Key = T -> identity, requires
//        T to be hashable + equality-comparable.
//
//    PGR-2 (group identity). A group with a given key is created
//        lazily on first source insert under that key, removed when
//        the last item under that key is removed, and re-created if
//        a new item under that key arrives later. Identity is the
//        Group object's address; observers may bind to per-group
//        ObservableList<T> long-lived.
//
//    PGR-3 (source-driven). Source insert / remove / replace / reset
//        propagate to the affected groups: the matched group's
//        inner list mutates; the outer GroupedList emits Insert /
//        Remove of `Group` only when groups appear / disappear.
//
//    PGR-4 (order). Outer GroupedList orders groups by the
//        source position of each group's seed (the first source
//        item that ever entered that group, at the time of entry).
//        When the source inserts a new-group item between two
//        existing source positions p_left < p_right, the new
//        outer slot lands between the outer slots whose seed items
//        sit at p_left / p_right. This mirrors `DistinctList` PD-2
//        and matches what users expect from sectioned table views
//        (sections appear where their first row is). Inner list
//        ordering matches the source order of items in that group.
//
//        Note: once a group is created, the outer position of the
//        group is *frozen* relative to the other live groups. If
//        the seed item is later removed and another item under the
//        same key remains, the surviving items keep the group at
//        its current outer slot rather than re-anchoring to the new
//        earliest member. This keeps the outer event stream stable
//        (no spurious Move events) and aligns with the DistinctList
//        promote-into-same-slot behaviour (PD-3 Replace).
//
//    PGR-5 (lifetime). Source destruction is safe (weak source
//        observer); all surviving Group objects continue to answer
//        from their cached inner lists.
//
//    PGR-6 (ItemChanged with key change). When T's `on_changed`
//        fires AND the new key differs from the current group, the
//        item is removed from the old group and re-inserted into
//        (or registered into) the new group. The outer list emits
//        Remove + Insert if a group disappears / appears as a
//        result; otherwise no outer event.
// ============================================================================
#pragma once

#include "aria/inplace_function.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/list_signal_mixin.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria {

/// One bucket in a `GroupedList<T, Key>`. Holds the key and an
/// `ObservableList<T>` of items currently under that key. The inner
/// list is the bind target for adapters (e.g. one section in a
/// sectioned table view).
template<typename T, typename Key>
struct Group {
    Key                                key;
    std::shared_ptr<ObservableList<T>> items;
};

template<typename T, typename Key = T>
class GroupedList
    : public detail::ListSignalMixin<GroupedList<T, Key>, Group<T, Key>> {
    friend detail::ListSignalMixin<GroupedList<T, Key>, Group<T, Key>>;

public:
    using value_type = Group<T, Key>;
    /// Owning, heap-free key extractor (capacity 32 bytes).
    using KeyOf      = aria::inplace_function<Key(const T&), 32>;
    using Signal     = detail::TypedSignal<ListChange<Group<T, Key>>>;

    GroupedList(std::shared_ptr<ObservableList<T>> source,
                KeyOf key_of = default_key_of_())
        : source_(std::move(source)),
          signal_(std::make_shared<Signal>()),
          state_(std::make_shared<SharedState>())
    {
        state_->key_of = std::move(key_of);
        rebuild_initial_();

        std::weak_ptr<SharedState>       weak_state  = state_;
        std::weak_ptr<Signal>            weak_signal = signal_;
        std::weak_ptr<ObservableList<T>> weak_source{source_};
        source_sub_ = source_->observe(
            [weak_state, weak_signal, weak_source](const ListChange<T>& ch) {
                auto st  = weak_state.lock();
                auto sig = weak_signal.lock();
                auto src = weak_source.lock();
                if (!st || !sig || !src) return;
                handle_source_change_(*st, *sig, *src, ch);
            });
    }

    ~GroupedList() = default;

    GroupedList(const GroupedList&)            = delete;
    GroupedList& operator=(const GroupedList&) = delete;

    // ── Read surface ──────────────────────────────────────────────────
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lk(state_->m);
        return state_->groups.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] std::shared_ptr<Group<T, Key>> at(std::size_t idx) const {
        std::shared_lock lk(state_->m);
        return state_->groups.at(idx);
    }

    [[nodiscard]] std::vector<std::shared_ptr<Group<T, Key>>> snapshot() const {
        std::shared_lock lk(state_->m);
        return state_->groups;
    }

    /// Find a group by key (returns nullptr if no such group exists
    /// currently). Useful for adapters that want to render a fixed
    /// set of section headers.
    [[nodiscard]] std::shared_ptr<Group<T, Key>> find(const Key& k) const {
        std::shared_lock lk(state_->m);
        auto it = state_->by_key.find(k);
        if (it == state_->by_key.end()) return nullptr;
        if (it->second >= state_->groups.size()) return nullptr;
        return state_->groups[it->second];
    }

private:
    struct SharedState {
        mutable std::shared_mutex                              m;
        KeyOf                                                  key_of;
        // Outer list, in first-appearance order.
        std::vector<std::shared_ptr<Group<T, Key>>>            groups;
        // Reverse: which outer slot owns this key.
        std::unordered_map<Key, std::size_t>                   by_key;
        // For PGR-6: track the key of each currently-tracked source item
        // (by raw pointer identity). Lets us detect key changes on
        // ItemChanged.
        std::unordered_map<const T*, Key>                      item_key;
    };

    std::shared_ptr<ObservableList<T>> source_;
    std::shared_ptr<Signal>            signal_;
    std::shared_ptr<SharedState>       state_;
    Subscription                       source_sub_;

    static KeyOf default_key_of_() {
        return [](const T& v) -> Key {
            if constexpr (std::is_same_v<Key, T>) {
                return v;
            } else {
                static_assert(std::is_same_v<Key, T>,
                    "GroupedList<T, Key>: default key extractor needs Key == T. "
                    "Provide a custom KeyOf for differing Key.");
                return Key{};
            }
        };
    }

    void rebuild_initial_() {
        auto snap = source_->snapshot();
        std::unique_lock lk(state_->m);
        for (const auto& sp_ : snap) {
            const Key k = state_->key_of(*sp_);
            state_->item_key[sp_.get()] = k;
            auto it = state_->by_key.find(k);
            if (it == state_->by_key.end()) {
                auto g    = std::make_shared<Group<T, Key>>();
                g->key    = k;
                g->items  = std::make_shared<ObservableList<T>>();
                g->items->push_back(sp_);
                state_->by_key.emplace(k, state_->groups.size());
                state_->groups.push_back(std::move(g));
            } else {
                state_->groups[it->second]->items->push_back(sp_);
            }
        }
    }

    static void handle_source_change_(SharedState& st, Signal& sig,
                                      ObservableList<T>& src,
                                      const ListChange<T>& ch) {
        switch (ch.kind) {
        case ListChangeKind::Insert:      handle_insert_(st, sig, src, ch);       return;
        case ListChangeKind::Remove:      handle_remove_(st, sig, ch);            return;
        case ListChangeKind::Replace:     handle_replace_(st, sig, src, ch);      return;
        case ListChangeKind::ItemChanged: handle_item_changed_(st, sig, src, ch); return;
        case ListChangeKind::Reset:       handle_reset_(st, sig);                 return;
        case ListChangeKind::Move:        /* outer order unchanged */             return;
        }
    }

    static void handle_insert_(SharedState& st, Signal& sig,
                               ObservableList<T>& src, const ListChange<T>& ch) {
        // Need the live shared_ptr<T>; pull from source by index.
        auto sp_ = src.at(ch.index);
        const Key k = st.key_of(*sp_);

        std::optional<std::size_t> emit_outer_insert_at;
        std::shared_ptr<Group<T, Key>> emit_group;
        {
            std::unique_lock lk(st.m);
            st.item_key[sp_.get()] = k;
            auto it = st.by_key.find(k);
            if (it == st.by_key.end()) {
                // PGR-4: place the new group at the outer position
                // that mirrors the source position of its seed
                // item. Fast path: if the source insert is at the
                // tail, the new outer slot is at the outer tail.
                std::size_t outer_pos;
                if (ch.index >= src.size() - 1) {
                    outer_pos = st.groups.size();
                } else if (ch.index == 0) {
                    outer_pos = 0;
                } else {
                    outer_pos = outer_pos_for_new_group_(st, src, ch.index);
                }
                auto g    = std::make_shared<Group<T, Key>>();
                g->key    = k;
                g->items  = std::make_shared<ObservableList<T>>();
                g->items->push_back(sp_);
                // Adjust by_key indices for groups that shift right.
                for (auto& [kk, pos] : st.by_key) {
                    if (pos >= outer_pos) ++pos;
                }
                st.by_key.emplace(k, outer_pos);
                emit_outer_insert_at = outer_pos;
                emit_group           = g;
                st.groups.insert(
                    st.groups.begin()
                        + static_cast<std::ptrdiff_t>(outer_pos),
                    std::move(g));
            } else {
                st.groups[it->second]->items->push_back(sp_);
            }
        }
        if (emit_outer_insert_at.has_value()) {
            sig.emit(ListChange<Group<T, Key>>{
                ListChangeKind::Insert, *emit_outer_insert_at,
                emit_group.get(), 0});
        }
    }

    /// Count how many distinct groups currently have at least one
    /// item at source positions strictly before `source_idx`. That
    /// count IS the outer position where the new-group seed belongs.
    /// O(N_source) walk; not on the hot path because Insert at the
    /// tail (the common case) takes the fast path above.
    static std::size_t outer_pos_for_new_group_(SharedState& st,
                                                ObservableList<T>& src,
                                                std::size_t source_idx) {
        std::unordered_map<Key, bool> seen;
        const std::size_t bound =
            std::min<std::size_t>(source_idx, src.size());
        for (std::size_t i = 0; i < bound; ++i) {
            auto sp_ = src.at(i);
            auto kit = st.item_key.find(sp_.get());
            if (kit == st.item_key.end()) continue;
            seen.emplace(kit->second, true);
        }
        return seen.size();
    }

    static void handle_remove_(SharedState& st, Signal& sig,
                               const ListChange<T>& ch) {
        if (ch.item == nullptr) return;
        std::optional<std::size_t> emit_outer_remove_at;
        std::shared_ptr<Group<T, Key>> emit_group;
        {
            std::unique_lock lk(st.m);
            auto k_it = st.item_key.find(ch.item);
            if (k_it == st.item_key.end()) return;
            const Key k = k_it->second;
            st.item_key.erase(k_it);

            auto by_it = st.by_key.find(k);
            if (by_it == st.by_key.end()) return;
            const std::size_t outer_pos = by_it->second;
            auto& g = st.groups[outer_pos];

            // Remove the item from the inner list (linear scan; the
            // common case is small groups).
            const std::size_t inner_size = g->items->size();
            for (std::size_t i = 0; i < inner_size; ++i) {
                if (g->items->at(i).get() == ch.item) {
                    g->items->remove_at(i);
                    break;
                }
            }
            if (g->items->empty()) {
                emit_outer_remove_at = outer_pos;
                emit_group           = g;
                st.groups.erase(st.groups.begin()
                                    + static_cast<std::ptrdiff_t>(outer_pos));
                st.by_key.erase(by_it);
                for (auto& [_, pos] : st.by_key) {
                    if (pos > outer_pos) --pos;
                }
            }
        }
        if (emit_outer_remove_at.has_value()) {
            sig.emit(ListChange<Group<T, Key>>{
                ListChangeKind::Remove, *emit_outer_remove_at,
                emit_group.get(), 0});
        }
    }

    static void handle_replace_(SharedState& st, Signal& sig,
                                ObservableList<T>& src,
                                const ListChange<T>& ch) {
        // Treat as Remove(old) + Insert(new) on the affected
        // groups. The outer list may emit 0, 1 or 2 events.
        const T* old_raw = ch.item;
        // Get the new live shared_ptr from source.
        auto sp_new = src.at(ch.index);
        // Synthesize the change events the inner handlers need.
        ListChange<T> remove_ch{ListChangeKind::Remove, ch.index, old_raw, 0};
        ListChange<T> insert_ch{ListChangeKind::Insert, ch.index, sp_new.get(), 0};
        handle_remove_(st, sig, remove_ch);
        handle_insert_(st, sig, src, insert_ch);
    }

    static void handle_item_changed_(SharedState& st, Signal& sig,
                                     ObservableList<T>& src,
                                     const ListChange<T>& ch) {
        // PGR-6: detect key change.
        if (ch.item == nullptr) return;
        Key old_key{};
        Key new_key{};
        {
            std::shared_lock lk(st.m);
            auto it = st.item_key.find(ch.item);
            if (it == st.item_key.end()) return;
            old_key = it->second;
            new_key = st.key_of(*ch.item);
        }
        if (old_key == new_key) {
            // Inner-list ItemChanged is already emitted by
            // ObservableList through the inner ObservableList. Outer
            // observers see no event.
            return;
        }
        // Re-route the item: remove from old group, insert into new.
        ListChange<T> remove_ch{ListChangeKind::Remove, ch.index, ch.item, 0};
        handle_remove_(st, sig, remove_ch);
        ListChange<T> insert_ch{ListChangeKind::Insert, ch.index, ch.item, 0};
        handle_insert_(st, sig, src, insert_ch);
    }

    static void handle_reset_(SharedState& st, Signal& sig) {
        {
            std::unique_lock lk(st.m);
            st.groups.clear();
            st.by_key.clear();
            st.item_key.clear();
        }
        sig.emit(ListChange<Group<T, Key>>{
            ListChangeKind::Reset, 0, nullptr, 0});
    }
};

}  // namespace aria
