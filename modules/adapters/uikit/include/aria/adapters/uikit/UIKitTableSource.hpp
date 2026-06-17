#pragma once

// UIKitTableSource.hpp — bridge any aria list source onto UITableView.
//
// UIKit counterpart of `qt_list_model_adapter.hpp` and
// `AppKitTableSource.hpp`. Accepts any source satisfying
// `aria::ListSourceOf<L, T>` (ObservableList / FilteredList /
// SortedList / MappedList) and turns `ListChange<T>` events into:
//
//     Insert       -> [UITableView insertRowsAtIndexPaths:...:]
//     Remove       -> [UITableView deleteRowsAtIndexPaths:...:]
//     Replace      -> [UITableView reloadRowsAtIndexPaths:...:]
//     ItemChanged  -> [UITableView reloadRowsAtIndexPaths:...:]
//     Move         -> [UITableView moveRowAtIndexPath:toIndexPath:]
//     Reset        -> [UITableView reloadData]
//
// Header is .mm-only (UIKit imports). Header-only template — same
// distribution model as the AppKit / Qt6 adapters.
//
// Threading
// ---------
// UIKit table mutation MUST happen on the main thread. Off-main-thread
// changes are queued onto `dispatch_get_main_queue()`. The
// `shared_ptr<T>` for events that need it is RESOLVED at emit time
// (before the dispatch hop), mirroring the Qt / AppKit contract.
//
// Lifetime
// --------
// The bridge holds a non-owning ref to the source list (caller keeps
// it alive) and a non-owning weak ref to the `UITableView`.
// Destruction order:
//   1. detach `Subscription`,
//   2. mark `state_->detached` so any in-flight queued block bails,
//   3. zero the table's dataSource/delegate iff still our object.

#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#import <UIKit/UIKit.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// ─── ObjC data-source / delegate ────────────────────────────────────────

@interface AriaUITableDataSource : NSObject <UITableViewDataSource, UITableViewDelegate>
- (instancetype)initWithRowCount:(std::function<NSInteger()>)rowCountFn
                       cellForFn:(std::function<UITableViewCell*(UITableView*,
                                                                  NSIndexPath*)>)cellForFn;
@end

namespace aria::adapters::uikit {

template<typename T>
class ObservableTableSource {
public:
    /// Render callback: given a row's `shared_ptr<T>`, the table view
    /// and the index path being asked about, return the
    /// `UITableViewCell*` to display (typically via
    /// `[tableView dequeueReusableCellWithIdentifier:...]`).
    using CellForRowFn = std::function<UITableViewCell*(UITableView*,
                                                        std::shared_ptr<T>,
                                                        NSIndexPath*)>;

    /// Construct a binding between an aria list source and a
    /// `UITableView`. The source must outlive `*this`.
    template<class L>
        requires ::aria::ListSourceOf<L, T>
    ObservableTableSource(UITableView* tableView,
                          L& source,
                          CellForRowFn cell_for_row)
        : state_(std::make_shared<State>()) {
        state_->table        = tableView;
        state_->cell_for_row = std::move(cell_for_row);
        state_->snapshot     = source.snapshot();

        std::weak_ptr<State> weak_state = state_;

        auto row_count_fn = [weak_state]() -> NSInteger {
            if (auto s = weak_state.lock()) {
                return static_cast<NSInteger>(s->snapshot.size());
            }
            return 0;
        };
        auto cell_for_fn = [weak_state](UITableView* tv,
                                        NSIndexPath* indexPath) -> UITableViewCell* {
            auto s = weak_state.lock();
            if (!s) return nil;
            const NSInteger row = indexPath.row;
            if (row < 0
                || static_cast<std::size_t>(row) >= s->snapshot.size()) {
                return [[UITableViewCell alloc]
                            initWithStyle:UITableViewCellStyleDefault
                          reuseIdentifier:@"empty"];
            }
            auto item = s->snapshot[static_cast<std::size_t>(row)];
            if (!item) {
                return [[UITableViewCell alloc]
                            initWithStyle:UITableViewCellStyleDefault
                          reuseIdentifier:@"empty"];
            }
            return s->cell_for_row(tv, item, indexPath);
        };

        state_->ds = [[AriaUITableDataSource alloc]
                          initWithRowCount:std::move(row_count_fn)
                                 cellForFn:std::move(cell_for_fn)];
        tableView.dataSource = state_->ds;
        tableView.delegate   = state_->ds;
        [tableView reloadData];

        sub_ = source.observe(
            [weak_state, &source](const ::aria::ListChange<T>& ch) {
                auto s = weak_state.lock();
                if (!s) return;
                if (s->detached.load(std::memory_order_acquire)) return;

                std::shared_ptr<T> resolved;
                using K = ::aria::ListChangeKind;
                if (ch.kind == K::Insert
                    || ch.kind == K::Replace
                    || ch.kind == K::Move) {
                    if (ch.index < source.size()) {
                        resolved = source.at(ch.index);
                    }
                }

                apply_on_main_(weak_state, ch, std::move(resolved));
            });
    }

    ~ObservableTableSource() {
        sub_ = ::aria::Subscription{};
        if (state_) {
            state_->detached.store(true, std::memory_order_release);
            UITableView* tv = state_->table;
            if (tv) {
                if (tv.dataSource == state_->ds) tv.dataSource = nil;
                if (tv.delegate   == state_->ds) tv.delegate   = nil;
            }
        }
    }

    ObservableTableSource(const ObservableTableSource&)            = delete;
    ObservableTableSource& operator=(const ObservableTableSource&) = delete;

    [[nodiscard]] std::size_t row_count() const noexcept {
        return state_ ? state_->snapshot.size() : 0;
    }

    [[nodiscard]] std::shared_ptr<T> at(std::size_t i) const {
        if (!state_) return nullptr;
        if (i >= state_->snapshot.size()) return nullptr;
        return state_->snapshot[i];
    }

private:
    struct State {
        UITableView* __weak              table = nil;
        AriaUITableDataSource* __strong  ds    = nil;
        CellForRowFn                     cell_for_row;
        std::vector<std::shared_ptr<T>>  snapshot;
        std::atomic<bool>                detached{false};
    };

    static void apply_on_main_(std::weak_ptr<State> weak_state,
                               ::aria::ListChange<T> ch,
                               std::shared_ptr<T> resolved) {
        auto run_now = [weak_state, ch, resolved]() {
            auto s = weak_state.lock();
            if (!s) return;
            if (s->detached.load(std::memory_order_acquire)) return;
            apply_change_(*s, ch, resolved);
        };
        if ([NSThread isMainThread]) {
            run_now();
        } else {
            __block auto blk = run_now;
            dispatch_async(dispatch_get_main_queue(), ^{ blk(); });
        }
    }

    static NSIndexPath* ip_(std::size_t row) {
        return [NSIndexPath indexPathForRow:static_cast<NSInteger>(row)
                                  inSection:0];
    }

    static void apply_change_(State& s,
                              const ::aria::ListChange<T>& ch,
                              const std::shared_ptr<T>& resolved) {
        using K = ::aria::ListChangeKind;
        switch (ch.kind) {
        case K::Insert:      apply_insert_(s, ch.index, resolved);      return;
        case K::Remove:      apply_remove_(s, ch.index);                return;
        case K::Replace:     apply_replace_(s, ch.index, resolved);     return;
        case K::ItemChanged: apply_item_changed_(s, ch.index);          return;
        case K::Move:        apply_move_(s, ch.from_index, ch.index);   return;
        case K::Reset:       apply_reset_(s);                           return;
        }
    }

    static void apply_insert_(State& s,
                              std::size_t idx,
                              const std::shared_ptr<T>& item) {
        if (idx > s.snapshot.size()) idx = s.snapshot.size();
        s.snapshot.insert(s.snapshot.begin() + static_cast<std::ptrdiff_t>(idx),
                          item);
        if (!s.table) return;
        [s.table insertRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationFade];
    }

    static void apply_remove_(State& s, std::size_t idx) {
        if (idx >= s.snapshot.size()) return;
        s.snapshot.erase(s.snapshot.begin() + static_cast<std::ptrdiff_t>(idx));
        if (!s.table) return;
        [s.table deleteRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationFade];
    }

    static void apply_replace_(State& s,
                               std::size_t idx,
                               const std::shared_ptr<T>& item) {
        if (idx >= s.snapshot.size()) return;
        s.snapshot[idx] = item;
        if (!s.table) return;
        [s.table reloadRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationFade];
    }

    static void apply_item_changed_(State& s, std::size_t idx) {
        if (idx >= s.snapshot.size()) return;
        if (!s.table) return;
        [s.table reloadRowsAtIndexPaths:@[ ip_(idx) ]
                       withRowAnimation:UITableViewRowAnimationNone];
    }

    static void apply_move_(State& s, std::size_t from, std::size_t to) {
        if (from == to) return;
        if (from >= s.snapshot.size() || to >= s.snapshot.size()) return;
        auto moved = s.snapshot[from];
        s.snapshot.erase(s.snapshot.begin() + static_cast<std::ptrdiff_t>(from));
        s.snapshot.insert(s.snapshot.begin() + static_cast<std::ptrdiff_t>(to),
                          std::move(moved));
        if (!s.table) return;
        [s.table moveRowAtIndexPath:ip_(from) toIndexPath:ip_(to)];
    }

    static void apply_reset_(State& s) {
        // Drop our snapshot and reload the table. The source will
        // subsequently emit Inserts to repopulate (this matches
        // `ObservableList::clear`, which leaves the list empty and
        // emits a single Reset event).
        s.snapshot.clear();
        if (s.table) [s.table reloadData];
    }

    std::shared_ptr<State> state_;
    ::aria::Subscription   sub_;
};

}  // namespace aria::adapters::uikit
