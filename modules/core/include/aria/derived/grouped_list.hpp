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
#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/list_signal_mixin.hpp"

#include <cstddef>
#include <optional>
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

template<typename T, typename Key = T,
         typename Source = ObservableList<T>>
    requires ListSourceOf<Source, T>
class GroupedList
    : public detail::ListSignalMixin<GroupedList<T, Key, Source>,
                                     Group<T, Key>> {
    friend detail::ListSignalMixin<GroupedList<T, Key, Source>, Group<T, Key>>;

public:
    using value_type = Group<T, Key>;
    /// Owning, heap-free key extractor (capacity 32 bytes).
    using KeyOf      = aria::inplace_function<Key(const T&), 32>;
    using Signal     = detail::ListSignal<Group<T, Key>>;

    GroupedList(std::shared_ptr<Source> source,
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
    struct SourceRow {
        std::shared_ptr<T> item;
        Key key;
    };

    struct InputChange {
        ListChangeKind kind;
        std::size_t index;
        std::size_t from;
        std::optional<SourceRow> row;
        std::vector<SourceRow> reset;
    };

    struct SharedState {
        mutable std::shared_mutex m;
        KeyOf key_of;
        std::vector<std::shared_ptr<Group<T, Key>>> groups;
        std::unordered_map<Key, std::size_t> by_key;
        // Source slots, including repeated handles. Replace carries the NEW
        // pointer, so its old group/position must come from this cache.
        std::vector<SourceRow> rows;
    };

    std::shared_ptr<Source> source_;
    std::shared_ptr<Signal> signal_;
    std::shared_ptr<SharedState> state_;
    Subscription source_sub_;

    static KeyOf default_key_of_() {
        return [](const T& v) -> Key {
            static_assert(std::is_same_v<Key, T>,
                "GroupedList: provide a key extractor when Key differs from T.");
            return v;
        };
    }

    static std::vector<SourceRow> source_rows_(SharedState& st, Source& src) {
        std::vector<SourceRow> rows;
        for (auto& item : src.snapshot()) {
            rows.push_back(SourceRow{item, st.key_of(*item)});
        }
        return rows;
    }

    static void rebuild_(SharedState& st, std::vector<SourceRow> rows) {
        std::vector<std::shared_ptr<Group<T, Key>>> groups;
        std::unordered_map<Key, std::size_t> by_key;
        for (const auto& row : rows) {
            auto [it, inserted] = by_key.emplace(row.key, groups.size());
            if (inserted) {
                groups.push_back(std::make_shared<Group<T, Key>>(
                    Group<T, Key>{row.key, std::make_shared<ObservableList<T>>()}));
            }
            // These new inner lists have no observers yet.
            groups[it->second]->items->push_back(row.item);
        }
        std::unique_lock lk(st.m);
        st.rows = std::move(rows);
        st.groups = std::move(groups);
        st.by_key = std::move(by_key);
    }

    void rebuild_initial_() {
        rebuild_(*state_, source_rows_(*state_, *source_));
    }

    static void handle_source_change_(SharedState& st, Signal& sig,
                                      Source& src, const ListChange<T>& ch) {
        InputChange event{ch.kind, ch.index, ch.from_index, {}, {}};
        if (ch.kind == ListChangeKind::Insert ||
            ch.kind == ListChangeKind::Replace ||
            ch.kind == ListChangeKind::ItemChanged) {
            auto item = ch.item;
            event.row.emplace(SourceRow{item, st.key_of(*item)});
        } else if (ch.kind == ListChangeKind::Reset) {
            for (const auto& item : *ch.snapshot) event.reset.push_back(SourceRow{item, st.key_of(*item)});
        }
        apply_(st, sig, std::move(event));
    }

    // Caller holds st.m. Inner positions follow source slots, not pointer
    // lookup, so two occurrences of the same shared_ptr remain distinct.
    static std::size_t inner_position_(const SharedState& st,
                                       const Key& key, std::size_t before) {
        std::size_t count = 0;
        for (std::size_t i = 0; i < before; ++i) {
            if (st.rows[i].key == key) ++count;
        }
        return count;
    }

    static void insert_(SharedState& st, Signal& sig,
                        std::size_t index, SourceRow row) {
        std::shared_ptr<Group<T, Key>> group;
        std::size_t inner = 0;
        std::size_t outer = 0;
        bool created = false;
        {
            std::unique_lock lk(st.m);
            const bool append = index == st.rows.size();
            auto found = st.by_key.find(row.key);
            if (found == st.by_key.end()) {
                outer = st.groups.size();
                if (!append) {
                    std::unordered_map<Key, bool> preceding;
                    for (std::size_t i = 0; i < index; ++i) {
                        preceding.emplace(st.rows[i].key, true);
                    }
                    outer = preceding.size();
                }
                group = std::make_shared<Group<T, Key>>(
                    Group<T, Key>{row.key, std::make_shared<ObservableList<T>>()});
                if (!append) {
                    for (auto& [key, position] : st.by_key) {
                        if (position >= outer) ++position;
                    }
                }
                st.by_key.emplace(row.key, outer);
                st.groups.insert(st.groups.begin() + static_cast<std::ptrdiff_t>(outer), group);
                created = true;
            } else {
                group = st.groups[found->second];
                // Source events are serialized through the complete inner
                // notification. A tail insert therefore follows every member
                // already in this group, including repeated handles.
                inner = append ? group->items->size()
                               : inner_position_(st, row.key, index);
            }
            st.rows.insert(st.rows.begin() + static_cast<std::ptrdiff_t>(index), row);
        }
        group->items->insert(inner, std::move(row.item));
        if (created) {
            sig.emit(ListChange<Group<T, Key>>{ListChangeKind::Insert, outer, group, 0});
        }
    }

    static void remove_(SharedState& st, Signal& sig, std::size_t index) {
        std::shared_ptr<Group<T, Key>> group;
        std::size_t inner;
        {
            std::unique_lock lk(st.m);
            const auto& row = st.rows.at(index);
            inner = inner_position_(st, row.key, index);
            group = st.groups[st.by_key.at(row.key)];
            st.rows.erase(st.rows.begin() + static_cast<std::ptrdiff_t>(index));
        }
        // ObservableList mutators synchronously notify their own observers.
        // Never call them under st.m, even when the group will disappear.
        group->items->remove_at(inner);
        if (group->items->empty()) {
            std::size_t outer;
            {
                std::unique_lock lk(st.m);
                outer = st.by_key.at(group->key);
                st.by_key.erase(group->key);
                st.groups.erase(st.groups.begin() + static_cast<std::ptrdiff_t>(outer));
                for (auto& [key, position] : st.by_key) {
                    if (position > outer) --position;
                }
            }
            sig.emit(ListChange<Group<T, Key>>{ListChangeKind::Remove, outer, group, 0});
        }
    }

    static void apply_(SharedState& st, Signal& sig, InputChange event) {
        if (event.kind == ListChangeKind::ItemChanged) {
            std::vector<std::size_t> occurrences;
            {
                std::shared_lock lk(st.m);
                for (std::size_t i = 0; i < st.rows.size(); ++i) {
                    if (st.rows[i].item == event.row->item && st.rows[i].key != event.row->key) {
                        occurrences.push_back(i);
                    }
                }
            }
            for (const auto index : occurrences) {
                remove_(st, sig, index);
                insert_(st, sig, index, *event.row);
            }
            return;
        }
        switch (event.kind) {
        case ListChangeKind::Insert:
            insert_(st, sig, event.index, std::move(*event.row));
            return;
        case ListChangeKind::Remove:
            remove_(st, sig, event.index);
            return;
        case ListChangeKind::Replace:
        case ListChangeKind::ItemChanged: {
            std::shared_ptr<Group<T, Key>> group;
            std::size_t inner = 0;
            {
                std::unique_lock lk(st.m);
                auto& old = st.rows.at(event.index);
                if (old.key == event.row->key) {
                    inner = inner_position_(st, old.key, event.index);
                    group = st.groups[st.by_key.at(old.key)];
                    old = *event.row;
                }
            }
            if (group) {
                if (event.kind == ListChangeKind::Replace) {
                    group->items->replace_at(inner, std::move(event.row->item));
                }
                // Same-key ItemChanged is emitted by the inner list's own
                // item subscription; forwarding here would double-notify.
            } else {
                remove_(st, sig, event.index);
                insert_(st, sig, event.index, std::move(*event.row));
            }
            return;
        }
        case ListChangeKind::Move: {
            std::shared_ptr<Group<T, Key>> group;
            std::size_t from;
            std::size_t to;
            {
                std::unique_lock lk(st.m);
                auto row = st.rows.at(event.from);
                group = st.groups[st.by_key.at(row.key)];
                from = inner_position_(st, row.key, event.from);
                st.rows.erase(st.rows.begin() + static_cast<std::ptrdiff_t>(event.from));
                to = inner_position_(st, row.key, event.index);
                st.rows.insert(st.rows.begin() + static_cast<std::ptrdiff_t>(event.index), std::move(row));
            }
            group->items->move(from, to);
            return;
        }
        case ListChangeKind::Reset:
            rebuild_(st, std::move(event.reset));
            {
                std::shared_lock lock(st.m);
                auto snapshot = st.groups;
                lock.unlock();
                sig.emit(ListChange<Group<T, Key>>::reset(std::move(snapshot)));
            }
            return;
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
[[nodiscard]] std::shared_ptr<GroupedList<T, Key, Source>>
grouped(std::shared_ptr<Source> source, KeyFn key_of) {
    using Derived = GroupedList<T, Key, Source>;
    return std::make_shared<Derived>(
        std::move(source), typename Derived::KeyOf{std::move(key_of)});
}

}  // namespace aria
