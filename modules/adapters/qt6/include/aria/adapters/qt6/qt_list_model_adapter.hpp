#pragma once

// ObservableListModel<T> — Qt6 model adapter for any aria list source.
//
// Bridges the four observable list types onto `QAbstractListModel`:
//
//   * `aria::ObservableList<T>`
//   * `aria::FilteredList<T>`
//   * `aria::SortedList<T>`
//   * `aria::MappedList<S, T>`     (element type T = `Target`)
//
// All four expose the same `ListChange<T>` vocabulary and the same
// read surface, so this adapter consumes them via the `aria::ListSource`
// concept rather than through inheritance. The `Move` event is bridged
// to Qt's `beginMoveRows` / `endMoveRows` so business code never has
// to redraw a derived list on its own.
//
// Usage — with a domain ObservableList:
//
//   ObservableListModel<Movie> model{vm.movies, roles, role_fn};
//
// Usage — with a derived (Filtered/Sorted/Mapped) list (recommended
// pattern: keep the derived list owned by the ViewModel, hand a
// reference to the model adapter):
//
//   auto active = std::make_shared<aria::FilteredList<Movie>>(
//       vm.movies_shared(), [](const Movie& m){ return m.year >= 2000; });
//   ObservableListModel<Movie> model{*active, roles, role_fn};
//
// The model holds a non-owning pointer to the source — the source must
// outlive the model.

#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QMetaObject>
#include <QModelIndex>
#include <QThread>
#include <QVariant>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace aria::adapters::qt6 {

template<typename T>
class ObservableListModel : public QAbstractListModel {
public:
    using RoleMap = QHash<int, QByteArray>;
    using RoleFn  = std::function<QVariant(const T&, int role)>;

    /// Generic constructor: accepts any list source whose element
    /// type matches `T`. The source is held by raw pointer; lifetime
    /// is the caller's responsibility.
    template<class L>
        requires ::aria::ListSourceOf<L, T>
    ObservableListModel(L& source,
                        RoleMap roles,
                        RoleFn role_fn,
                        QObject* parent = nullptr)
        : QAbstractListModel(parent),
          roles_(std::move(roles)),
          role_fn_(std::move(role_fn)),
          snapshot_(source.snapshot()),
          size_fn_([&source]() { return source.size(); }),
          at_fn_([&source](std::size_t i) { return source.at(i); }) {
        // The observe callback may fire on ANY thread (some producers push
        // from worker threads). Qt's model APIs (beginInsertRows etc.)
        // must only be called on the thread that owns `this`. We bounce
        // the change to our owning thread via QMetaObject::invokeMethod,
        // which queues it to this QObject's event loop.
        //
        // IMPORTANT: by the time the queued lambda runs, the source may
        // have mutated further. We capture whatever extra data the
        // change applier needs RIGHT NOW — while the change is fresh —
        // instead of calling `source.at(ch.index)` from inside the
        // queued lambda. Otherwise rapid back-to-back mutations observe
        // an inconsistent view.
        sub_ = source.observe([this](const ::aria::ListChange<T>& ch) {
            std::shared_ptr<T> resolved;
            using K = ::aria::ListChangeKind;
            if (ch.kind == K::Insert
                || ch.kind == K::Replace
                || ch.kind == K::Move) {
                if (ch.index < size_fn_()) {
                    resolved = at_fn_(ch.index);
                }
            }

            if (QThread::currentThread() == this->thread()) {
                apply_change_(ch, std::move(resolved));
            } else {
                auto captured = std::move(resolved);
                QMetaObject::invokeMethod(this, [this, ch, captured]() {
                    apply_change_(ch, captured);
                }, Qt::QueuedConnection);
            }
        });
    }

    int rowCount(const QModelIndex& parent = QModelIndex{}) const override {
        if (parent.isValid()) return 0;
        return static_cast<int>(snapshot_.size());
    }

    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override {
        if (!index.isValid()) return {};
        auto row = index.row();
        if (row < 0 || row >= rowCount()) return {};
        auto item = snapshot_[static_cast<std::size_t>(row)];
        if (!item) return {};
        return role_fn_(*item, role);
    }

    QHash<int, QByteArray> roleNames() const override {
        return roles_;
    }

    /// Rebuild snapshot from the source list and emit model reset.
    /// Falls back to using the captured `size_fn_` / `at_fn_` so a
    /// reload also works for derived lists.
    void reload() {
        beginResetModel();
        const auto n = size_fn_();
        snapshot_.clear();
        snapshot_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            snapshot_.push_back(at_fn_(i));
        }
        endResetModel();
    }

private:
    /// Apply a single change to the local snapshot and emit the matching
    /// Qt model signals. `resolved_item` is the item that was live at the
    /// moment the change was emitted on the source list — passed in so a
    /// racy `source.at(ch.index)` inside this function (which may run
    /// arbitrarily later on the queued event loop) cannot see a different
    /// value than the one we promised observers.
    ///
    /// For Remove / ItemChanged / Reset we don't need the resolved item;
    /// the snapshot already knows what to evict, and ItemChanged just
    /// re-renders using `data()` which reads from `snapshot_`.
    void apply_change_(const ::aria::ListChange<T>& ch,
                       std::shared_ptr<T> resolved_item = {}) {
        switch (ch.kind) {
        case ::aria::ListChangeKind::Insert: {
            auto row = static_cast<int>(ch.index);
            beginInsertRows(QModelIndex{}, row, row);
            snapshot_.insert(snapshot_.begin() + static_cast<std::ptrdiff_t>(ch.index),
                             std::move(resolved_item));
            endInsertRows();
            break;
        }
        case ::aria::ListChangeKind::Remove: {
            auto row = static_cast<int>(ch.index);
            if (ch.index >= snapshot_.size()) return;
            beginRemoveRows(QModelIndex{}, row, row);
            snapshot_.erase(snapshot_.begin() + static_cast<std::ptrdiff_t>(ch.index));
            endRemoveRows();
            break;
        }
        case ::aria::ListChangeKind::Replace:
        case ::aria::ListChangeKind::ItemChanged: {
            if (ch.index >= snapshot_.size()) return;
            if (ch.kind == ::aria::ListChangeKind::Replace) {
                snapshot_[ch.index] = std::move(resolved_item);
            }
            auto idx = createIndex(static_cast<int>(ch.index), 0);
            Q_EMIT dataChanged(idx, idx, roles_.keys());
            break;
        }
        case ::aria::ListChangeKind::Move: {
            // Translate (from, to) into Qt's beginMoveRows contract.
            // Qt's `destinationChild` is the index in the FINAL layout
            // where the moved row should appear *as if the source were
            // still there* — i.e. for downward moves you pass `to + 1`.
            // See QAbstractItemModel::beginMoveRows for details.
            if (ch.from_index >= snapshot_.size()
                || ch.index >= snapshot_.size()
                || ch.from_index == ch.index) return;

            const auto from = static_cast<int>(ch.from_index);
            const auto to   = static_cast<int>(ch.index);
            const int  dest = (to > from) ? to + 1 : to;

            beginMoveRows(QModelIndex{}, from, from, QModelIndex{}, dest);
            auto moved = snapshot_[ch.from_index];
            snapshot_.erase(snapshot_.begin()
                                + static_cast<std::ptrdiff_t>(ch.from_index));
            snapshot_.insert(snapshot_.begin()
                                + static_cast<std::ptrdiff_t>(ch.index),
                             std::move(moved));
            endMoveRows();
            break;
        }
        case ::aria::ListChangeKind::Reset:
            reload();
            break;
        }
    }

    RoleMap                          roles_;
    RoleFn                           role_fn_;
    std::vector<std::shared_ptr<T>>  snapshot_;
    std::function<std::size_t()>            size_fn_;
    std::function<std::shared_ptr<T>(std::size_t)> at_fn_;
    ::aria::Subscription             sub_;
};

}  // namespace aria::adapters::qt6
