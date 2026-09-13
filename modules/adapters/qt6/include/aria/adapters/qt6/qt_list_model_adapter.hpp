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
// Construction snapshots and observes the source on its graph thread.
// Subsequent owning events are queued to the model's Qt owner thread;
// the model never reads the source while replaying them.

#include "aria/callback_boundary.hpp"
#include "aria/list_source.hpp"
#include "aria/observable_list.hpp"
#include "aria/subscription.hpp"

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QMetaObject>
#include <QModelIndex>
#include <QPointer>
#include <QThread>
#include <QVariant>

#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
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
    /// type matches `T`. Source reads happen only during construction;
    /// subsequent Insert/Replace/Reset events own the replayed payload.
    template<class L>
        requires ::aria::ListSourceOf<L, T>
    ObservableListModel(L& source,
                        RoleMap roles,
                        RoleFn role_fn,
                        QObject* parent = nullptr)
        : QAbstractListModel(parent),
          roles_(std::move(roles)),
          role_fn_(std::make_shared<RoleFn>(std::move(role_fn))),
          snapshot_(source.snapshot()) {
        check_size_(snapshot_.size());
        delivery_->model = this;
        std::weak_ptr<Delivery> weak = delivery_;
        sub_ = source.observe([weak](const ::aria::ListChange<T>& change) {
            if (auto delivery = weak.lock()) enqueue_(delivery, change);
        });
    }

    ~ObservableListModel() override {
        Q_ASSERT(QThread::currentThread() == thread());
        std::deque<::aria::ListChange<T>> retired;
        {
            std::lock_guard lock(delivery_->mutex);
            delivery_->model = nullptr;
            retired.swap(delivery_->pending);
        }
        // Both subscriptions and payloads can release application objects.
        // Their destructors must run after the lifetime lock is released.
        sub_.release();
    }

    int rowCount(const QModelIndex& parent = QModelIndex{}) const override {
        if (parent.isValid()) return 0;
        return static_cast<int>(snapshot_.size());
    }

    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override {
        if (!index.isValid() || index.model() != this || index.column() != 0) return {};
        const auto row = index.row();
        if (row < 0 || row >= rowCount()) return {};
        auto item = snapshot_[static_cast<std::size_t>(row)];
        if (!item) return {};
        try {
            // A role callback can release the model itself. Keep its target
            // and the item alive independently until invocation returns.
            auto project = role_fn_;
            return (*project)(*item, role);
        } catch (...) {
            ::aria::report_callback_failure("qt.list_model.role", std::current_exception());
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override {
        return roles_;
    }

    /// Re-notify Qt views using the current event-maintained snapshot.
    /// No source lookup is needed, so queued events cannot be duplicated.
    void reload() {
        Q_ASSERT(QThread::currentThread() == thread());
        QPointer<ObservableListModel> alive(this);
        beginResetModel();
        if (alive) endResetModel();
    }

private:
    struct Delivery {
        std::mutex mutex;
        ObservableListModel* model = nullptr;
        std::deque<::aria::ListChange<T>> pending;
        bool scheduled = false;
    };

    static void check_size_(std::size_t size) {
        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            throw std::length_error("ObservableListModel: row count exceeds Qt's int range");
    }

    static void enqueue_(const std::shared_ptr<Delivery>& delivery,
                         const ::aria::ListChange<T>& change) {
        std::unique_lock lock(delivery->mutex);
        auto* model = delivery->model;
        if (!model) return;
        delivery->pending.push_back(change);
        if (delivery->scheduled) return;
        delivery->scheduled = true;
        if (QThread::currentThread() == model->thread()) {
            lock.unlock();
            drain_(delivery);
        } else {
            // Lifetime lock prevents QObject destruction while registering
            // the delivery. Qt drops the functor if its context dies later.
            QMetaObject::invokeMethod(model, [weak = std::weak_ptr<Delivery>(delivery)] {
                if (auto current = weak.lock()) drain_(current);
            }, Qt::QueuedConnection);
        }
    }

    static void drain_(const std::shared_ptr<Delivery>& delivery) noexcept {
        for (;;) {
            ::aria::ListChange<T> change{};
            ObservableListModel* model;
            {
                std::lock_guard lock(delivery->mutex);
                model = delivery->model;
                if (!model || delivery->pending.empty()) {
                    delivery->scheduled = false;
                    return;
                }
                change = std::move(delivery->pending.front());
                delivery->pending.pop_front();
            }
            // Queuing also serialises reentrant model notifications, so a
            // second begin/end pair never interrupts the current change.
            try { model->apply_change_(change); }
            catch (...) {
                ::aria::report_callback_failure("qt.list_model.change", std::current_exception());
            }
        }
    }

    void apply_change_(const ::aria::ListChange<T>& ch) {
        Q_ASSERT(QThread::currentThread() == thread());
        QPointer<ObservableListModel> alive(this);
        switch (ch.kind) {
        case ::aria::ListChangeKind::Insert: {
            if (ch.index > snapshot_.size())
                throw std::out_of_range("ObservableListModel: invalid insert index");
            check_size_(snapshot_.size() + 1);
            // Reserve before beginInsertRows: allocation failure must not
            // leave Qt inside an unmatched structural notification pair.
            if (snapshot_.size() == snapshot_.capacity()) {
                const auto capacity = snapshot_.capacity();
                constexpr auto maximum = static_cast<std::size_t>(std::numeric_limits<int>::max());
                snapshot_.reserve(capacity > maximum / 2
                    ? maximum : (capacity == 0 ? 1 : capacity * 2));
            }
            auto row = static_cast<int>(ch.index);
            beginInsertRows(QModelIndex{}, row, row);
            if (!alive) return;
            snapshot_.insert(snapshot_.begin() + static_cast<std::ptrdiff_t>(ch.index),
                             ch.item);
            endInsertRows();
            break;
        }
        case ::aria::ListChangeKind::Remove: {
            auto row = static_cast<int>(ch.index);
            if (ch.index >= snapshot_.size()) return;
            beginRemoveRows(QModelIndex{}, row, row);
            if (!alive) return;
            snapshot_.erase(snapshot_.begin() + static_cast<std::ptrdiff_t>(ch.index));
            endRemoveRows();
            break;
        }
        case ::aria::ListChangeKind::Replace:
        case ::aria::ListChangeKind::ItemChanged: {
            if (ch.index >= snapshot_.size()) return;
            auto retired = std::move(snapshot_[ch.index]);
            snapshot_[ch.index] = ch.item;
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

            if (!beginMoveRows(QModelIndex{}, from, from, QModelIndex{}, dest) || !alive) return;
            auto moved = snapshot_[ch.from_index];
            snapshot_.erase(snapshot_.begin()
                                + static_cast<std::ptrdiff_t>(ch.from_index));
            snapshot_.insert(snapshot_.begin()
                                + static_cast<std::ptrdiff_t>(ch.index),
                             std::move(moved));
            endMoveRows();
            break;
        }
        case ::aria::ListChangeKind::Reset: {
            if (!ch.snapshot)
                throw std::invalid_argument("ObservableListModel: Reset requires its snapshot");
            check_size_(ch.snapshot->size());
            auto next = *ch.snapshot;
            beginResetModel();
            if (!alive) return;
            snapshot_.swap(next);
            endResetModel();
            break;
        }
        }
    }

    RoleMap                          roles_;
    std::shared_ptr<RoleFn>           role_fn_;
    std::vector<std::shared_ptr<T>>  snapshot_;
    std::shared_ptr<Delivery> delivery_ = std::make_shared<Delivery>();
    ::aria::Subscription             sub_;
};

}  // namespace aria::adapters::qt6
