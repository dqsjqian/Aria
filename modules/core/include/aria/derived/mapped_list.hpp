#pragma once

// MappedList<Source, Target> — a 1:1 projection of an ObservableList.
//
// Given a source list `ObservableList<Source>` and a mapper
// `Target(const Source&)` (returning `shared_ptr<Target>`), MappedList
// exposes a parallel list of Targets that tracks the source:
//
//   source: [Person("alice"), Person("bob"), ...]
//   mapper: [](const Person& p) { return std::make_shared<PersonVM>(p); }
//   ↓
//   mapped: [PersonVM("alice"), PersonVM("bob"), ...]
//
// A MappedList is the canonical way to turn a domain-level list into
// a view-model-level list without wiring per-row subscription glue by
// hand. The derived list emits the same ListChange<Target> vocabulary
// ObservableList / FilteredList / SortedList use, so adapters
// (QtListModel etc.) can consume it identically.
//
// Identity preservation:
//   `st.targets[i]` IS the authoritative cache — there is no
//   separate `Source* → Target` map. Move / ItemChanged just leave
//   the slot alone (or move it), so the Target pointer that was
//   live before is still live after. Replace / Remove drop the
//   slot; external shared_ptr refs keep the Target alive if the
//   caller still holds one.
//
// ItemChanged policy:
//   By default, ItemChanged propagates as `ItemChanged(j, target_ptr)`
//   *without* re-running the mapper. Rationale: Targets are usually
//   long-lived ViewModels that already subscribe to the Source
//   themselves (via `Source::on_changed` inside `PersonVM`). A
//   re-map would churn the identity for no benefit.
//
//   Call `MappedList<S,T>(src, mapper, /*remap_on_change=*/true)` to
//   invalidate the slot and re-invoke the mapper on every
//   ItemChanged — suitable when Target is a cheap, immutable
//   snapshot that must rebuild on each update.
//
// Event-translation contract:
//
//     Source event            Derived behaviour
//     ────────────────        ──────────────────────────────────────────
//     Insert(i, x)            Insert(i, mapper(x))
//     Remove(i)               Remove(i, old_target.get())
//     Replace(i, x_new)       Replace(i, mapper(x_new))
//     ItemChanged(i)          ItemChanged(i, target_ptr) by default;
//                             Replace(i, new_target) with remap_on_change
//     Move(from, to)          Move(from, to, target_ptr)
//     Reset                   Reset (fully rebuilt from the post-reset
//                             source snapshot)
//
// Thread-safety and lifetime: same shape as FilteredList / SortedList.

#include "aria/inplace_function.hpp"
#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"
#include "aria/detail/list_signal_mixin.hpp"
#include "aria/detail/typed_signal.hpp"
#include "aria/detail/list_replay.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace aria {

template<typename Source, typename Target,
         typename SourceList = ObservableList<Source>>
    requires ListSourceOf<SourceList, Source>
class MappedList
    : public detail::ListSignalMixin<MappedList<Source, Target, SourceList>,
                                     Target> {
    friend detail::ListSignalMixin<MappedList<Source, Target, SourceList>,
                                   Target>;

public:
    /// Element type *of the derived list* — i.e. `Target`. This is
    /// what adapter / `ListSource` consumers care about.
    using value_type = Target;
    /// Owning, heap-free mapper handle (capacity 32 bytes).
    using Mapper = aria::inplace_function<std::shared_ptr<Target>(const Source&), 32>;
    using Signal = detail::ListSignal<Target>;

    /// Construct a MappedList.
    ///
    /// @param source  the upstream list to project from
    /// @param mapper  `Target(const Source&)` — called once per source
    ///                item on Insert / Replace / Reset (and also on
    ///                ItemChanged when `remap_on_change` is true)
    /// @param remap_on_change  if true, ItemChanged invalidates the
    ///                slot and re-invokes the mapper; if false
    ///                (default), ItemChanged propagates as
    ///                `ItemChanged` on the existing Target and the
    ///                Target identity is preserved
    MappedList(std::shared_ptr<SourceList> source,
               Mapper mapper,
               bool remap_on_change = false)
        : source_(std::move(source)),
          signal_(std::make_shared<Signal>()),
          state_(std::make_shared<SharedState>())
    {
        state_->mapper          = std::move(mapper);
        state_->remap_on_change = remap_on_change;

        // Initial snapshot — map each source item once.
        {
            std::unique_lock lk(state_->m);
            auto snap = source_->snapshot();
            state_->source_items = snap;
            state_->targets.reserve(snap.size());
            for (const auto& s : snap) {
                state_->targets.push_back(state_->mapper(*s));
            }
        }

        // Subscribe on source. Listener holds only weak_ptrs.
        std::weak_ptr<SharedState>            weak_state  = state_;
        std::weak_ptr<Signal>                 weak_signal = signal_;
        std::weak_ptr<SourceList> weak_source{source_};
        source_sub_ = source_->observe(
            [weak_state, weak_signal, weak_source](const ListChange<Source>& ch) {
                auto st  = weak_state.lock();
                auto sig = weak_signal.lock();
                auto src = weak_source.lock();
                if (!st || !sig || !src) return;
                dispatch_source_change_(*st, *sig, ch);
            });
    }

    ~MappedList() = default;

    MappedList(const MappedList&)            = delete;
    MappedList& operator=(const MappedList&) = delete;

    // ── Read surface ──────────────────────────────────────────────────
    [[nodiscard]] std::size_t size() const {
        std::shared_lock lk(state_->m);
        return state_->targets.size();
    }

    [[nodiscard]] bool empty() const { return size() == 0; }

    [[nodiscard]] std::shared_ptr<Target> at(std::size_t idx) const {
        std::shared_lock lk(state_->m);
        return state_->targets.at(idx);
    }

    [[nodiscard]] std::vector<std::shared_ptr<Target>> snapshot() const {
        std::shared_lock lk(state_->m);
        return state_->targets;
    }

private:
    struct SharedState {
        mutable std::shared_mutex            m;
        Mapper                               mapper;
        bool                                 remap_on_change{false};
        std::vector<std::shared_ptr<Target>> targets;
        // The source serializes its event stream. Retain a replay mirror so
        // a failed mapper can recover from the next event, without reading
        // a source snapshot that may already include a later batch edit.
        std::vector<std::shared_ptr<Source>> source_items;
        bool needs_rebuild = false;
    };

    std::shared_ptr<SourceList> source_;
    std::shared_ptr<Signal>                 signal_;
    std::shared_ptr<SharedState>            state_;
    Subscription                            source_sub_;

    static void dispatch_source_change_(SharedState& st,
                                        Signal& sig,
                                        const ListChange<Source>& ch) {
        detail::replay_list_change(st.source_items, ch);
        if (st.needs_rebuild) {
            handle_reset_(st, sig, ListChange<Source>::reset(st.source_items));
            st.needs_rebuild = false;
            return;
        }
        // If a mapper/allocation fails, the ABI boundary reports it. Preserve
        // a valid old projection and rebuild before interpreting more indices.
        st.needs_rebuild = true;
        switch (ch.kind) {
        case ListChangeKind::Insert:      handle_insert_(st, sig, ch);       break;
        case ListChangeKind::Remove:      handle_remove_(st, sig, ch);       break;
        case ListChangeKind::Replace:     handle_replace_(st, sig, ch);      break;
        case ListChangeKind::ItemChanged: handle_item_changed_(st, sig, ch); break;
        case ListChangeKind::Move:        handle_move_(st, sig, ch);         break;
        case ListChangeKind::Reset:       handle_reset_(st, sig, ch);        break;
        }
        st.needs_rebuild = false;
    }

    static void handle_insert_(SharedState& st, Signal& sig,
                               const ListChange<Source>& ch) {
        auto t = st.mapper(*ch.item);
        {
            std::unique_lock lk(st.m);
            const std::size_t idx = ch.index;
            st.targets.insert(st.targets.begin()
                              + static_cast<std::ptrdiff_t>(idx), t);
        }
        sig.emit(ListChange<Target>{ListChangeKind::Insert, ch.index, t, 0});
    }

    static void handle_remove_(SharedState& st, Signal& sig,
                               const ListChange<Source>& ch) {
        std::shared_ptr<Target> removed;
        {
            std::unique_lock lk(st.m);
            const std::size_t idx = ch.index;
            if (idx >= st.targets.size()) return;
            removed = st.targets[idx];
            st.targets.erase(st.targets.begin()
                             + static_cast<std::ptrdiff_t>(idx));
        }
        sig.emit(ListChange<Target>{ListChangeKind::Remove, ch.index,
                                     removed, 0});
    }

    static void handle_replace_(SharedState& st, Signal& sig,
                                const ListChange<Source>& ch) {
        auto t = st.mapper(*ch.item);
        std::shared_ptr<Target> retired;
        {
            std::unique_lock lk(st.m);
            const std::size_t idx = ch.index;
            if (idx >= st.targets.size()) return;
            retired = std::exchange(st.targets[idx], t);
        }
        sig.emit(ListChange<Target>{ListChangeKind::Replace, ch.index,
                                     t, 0});
    }

    static void handle_item_changed_(SharedState& st, Signal& sig,
                                     const ListChange<Source>& ch) {
        auto t = st.remap_on_change ? st.mapper(*ch.item) : std::shared_ptr<Target>{};
        std::shared_ptr<Target> retired;
        {
            std::unique_lock lk(st.m);
            const std::size_t idx = ch.index;
            if (idx >= st.targets.size()) return;

            if (st.remap_on_change) {
                retired = std::exchange(st.targets[idx], t);
            } else {
                // Preserve Target identity; downstream observers that
                // want "refresh" semantics should subscribe to the
                // Source inside their Target.
                t = st.targets[idx];
            }
        }
        sig.emit(ListChange<Target>{st.remap_on_change ? ListChangeKind::Replace
                                                     : ListChangeKind::ItemChanged,
                                     ch.index, t, 0});
    }

    static void handle_move_(SharedState& st, Signal& sig,
                             const ListChange<Source>& ch) {
        std::shared_ptr<Target> moved;
        {
            std::unique_lock lk(st.m);
            const std::size_t from = ch.from_index;
            const std::size_t to   = ch.index;
            if (from == to) return;
            if (from >= st.targets.size() || to >= st.targets.size()) return;

            moved = st.targets[from];
            st.targets.erase(st.targets.begin()
                             + static_cast<std::ptrdiff_t>(from));
            st.targets.insert(st.targets.begin()
                              + static_cast<std::ptrdiff_t>(to), moved);
        }
        sig.emit(ListChange<Target>{ListChangeKind::Move, ch.index,
                                     moved, ch.from_index});
    }

    static void handle_reset_(SharedState& st, Signal& sig,
                              const ListChange<Source>& ch) {
        std::vector<std::shared_ptr<Target>> replacement;
        replacement.reserve(ch.snapshot->size());
        for (const auto& item : *ch.snapshot) {
            replacement.push_back(st.mapper(*item));
        }
        auto reset = ListChange<Target>::reset(replacement);
        {
            std::unique_lock lk(st.m);
            st.targets.swap(replacement);
        }
        sig.emit(std::move(reset));
        // Retired targets and their capture destructors run after unlocking.

    }
};

// ---------------------------------------------------------------------------
//  Factory helper — deduces the source type so pipelines stay readable.
//  See the note on `aria::filtered` in filtered_list.hpp.
// ---------------------------------------------------------------------------
template<typename Target,
         typename SourceList,
         typename MapFn,
         typename Source = list_source_value_t<SourceList>>
    requires ListSourceOf<SourceList, Source>
[[nodiscard]] std::shared_ptr<MappedList<Source, Target, SourceList>>
mapped(std::shared_ptr<SourceList> source,
       MapFn mapper,
       bool remap_on_change = false) {
    using Derived = MappedList<Source, Target, SourceList>;
    return std::make_shared<Derived>(
        std::move(source),
        typename Derived::Mapper{std::move(mapper)},
        remap_on_change);
}

}  // namespace aria
