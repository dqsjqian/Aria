#pragma once

#include "aria/callback_boundary.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QObject>
#include <QPointer>
#include <QThread>
#include <QVariant>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace aria::adapters::qt6 {

namespace detail {

template<typename Key, typename KeyFromData>
class ComboBoxSelectionBinding final : public QObject {
public:
    ComboBoxSelectionBinding(Property<std::optional<Key>>& selected,
                             QComboBox& combo, KeyFromData key_from_data,
                             int role)
        : selected_(&selected), combo_(&combo), model_(combo.model()),
          key_from_data_(std::move(key_from_data)), role_(role) {}

    void start(std::weak_ptr<ComboBoxSelectionBinding> weak) {
        // activated identifies user input. currentIndexChanged also fires
        // while Qt inserts/removes rows and chooses a fallback selection;
        // that automatic choice must never overwrite the ViewModel's ID.
        QObject::connect(combo_, &QComboBox::activated, this,
            [weak](int index) {
                if (auto state = weak.lock()) {
                    state->at_callback_boundary([&] { state->activate(index); });
                }
            });
        QObject::connect(combo_, &QObject::destroyed, this, [weak] {
            if (auto state = weak.lock()) state->stop();
        });
        QObject::connect(model_, &QObject::destroyed, this, [weak] {
            if (auto state = weak.lock()) state->stop();
        });

        const auto refresh = [weak] {
            if (auto state = weak.lock()) {
                state->at_callback_boundary([&] { state->synchronize(true); });
            }
        };
        QObject::connect(model_, &QAbstractItemModel::rowsInserted, this, refresh);
        QObject::connect(model_, &QAbstractItemModel::rowsRemoved, this, refresh);
        QObject::connect(model_, &QAbstractItemModel::rowsMoved, this, refresh);
        QObject::connect(model_, &QAbstractItemModel::dataChanged, this, refresh);
        QObject::connect(model_, &QAbstractItemModel::modelReset, this, refresh);
        QObject::connect(model_, &QAbstractItemModel::layoutChanged, this, refresh);

        selected_sub_ = selected_->on_changed([weak](const std::optional<Key>&) {
            if (auto state = weak.lock()) {
                state->at_callback_boundary([&] { state->synchronize(false); });
            }
        });
        synchronize(false);
    }

    void stop() noexcept {
        // Clear the widget first: a callback already in flight may retain
        // this state until it returns, but must perform no further work.
        combo_.clear();
        model_.clear();
        selected_sub_.release();
    }

private:
    template<typename Callback>
    static void at_callback_boundary(Callback&& callback) noexcept {
        try {
            std::forward<Callback>(callback)();
        } catch (...) {
            aria::report_callback_failure("qt.combo_box_selection",
                                          std::current_exception());
        }
    }

    void activate(int index) {
        if (!combo_ || !model_) return;
        std::optional<Key> next;
        if (index >= 0 && index < combo_->count()) {
            next = std::invoke(key_from_data_, combo_->itemData(index, role_));
        }
        resolved_key_ = next;
        selected_->set(std::move(next));
    }

    void synchronize(bool model_changed) {
        if (!combo_ || !model_) return;
        const auto key = selected_->peek();
        int row = -1;
        if (key) {
            for (int i = 0; i < combo_->count(); ++i) {
                if (std::invoke(key_from_data_, combo_->itemData(i, role_)) == *key) {
                    row = i;
                    break;
                }
            }
        }

        const bool removed = model_changed && key && resolved_key_ == key && row < 0;
        resolved_key_ = row >= 0 ? key : std::nullopt;
        if (removed) {
            selected_->set(std::nullopt);
            // Observers may choose a fallback ID, or notifications may be
            // batched. Apply the latest Property value in either case.
            synchronize(false);
            return;
        }
        combo_->setCurrentIndex(row);
    }

    Property<std::optional<Key>>* selected_;
    QPointer<QComboBox> combo_;
    QPointer<QAbstractItemModel> model_;
    KeyFromData key_from_data_;
    int role_;
    std::optional<Key> resolved_key_;
    Subscription selected_sub_;
};

}  // namespace detail

/// Bind a ViewModel's stable selected ID to a QComboBox's existing model.
/// `key_from_data(const QVariant&)` returns Key from the given item-data
/// role; it must be side-effect-free, with a unique ID for every option.
/// Conversion failures during callbacks are reported through the callback
/// failure sink; exceptions never escape into Qt's event loop. Initial
/// synchronization errors propagate to the caller of this function.
/// Labels may be duplicated. ObservableListModel can supply the options
/// directly, including a FilteredList or another derived list source.
///
/// The Property is authoritative at bind time. nullopt displays no row;
/// an unknown ID also displays no row but remains pending until its option
/// arrives. Removing/replacing an option whose ID was selected clears the
/// Property; reordering rows or replacing a row with the same ID keeps it.
///
/// Only user activation writes back to the Property. Programmatic changes
/// must write the Property, rather than call setCurrentIndex on the widget.
/// This prevents Qt's automatic row selection during model changes from
/// writing an unintended ID back into the ViewModel.
///
/// Call and release on the widget/model's Qt owner thread, which must also
/// be the Property's graph thread. Keep the Property and model/source alive
/// until the binding is released (or the combo is destroyed). Set the model
/// before binding; replacing the widget's model requires releasing and
/// rebinding. Releasing the returned Subscription disconnects both directions;
/// destroying the combo first is also safe.
template<typename Key, typename KeyFromData>
[[nodiscard]] Subscription bind_combo_box_selection(
    Property<std::optional<Key>>& selected, QComboBox& combo,
    KeyFromData key_from_data, int role = Qt::UserRole) {
    Q_ASSERT(QThread::currentThread() == combo.thread());
    Q_ASSERT(combo.model() && combo.model()->thread() == combo.thread());
    // An empty combo creates its popup lazily. That creation reconnects
    // Qt's model handlers, so complete it before installing our end-of-
    // change handlers; Qt's default selection must settle before we align.
    (void)combo.view();
    using State = detail::ComboBoxSelectionBinding<Key, KeyFromData>;
    auto state = std::make_shared<State>(selected, combo, std::move(key_from_data), role);
    state->start(state);
    return Subscription{std::function<void()>{[state] { state->stop(); }}};
}

}  // namespace aria::adapters::qt6
