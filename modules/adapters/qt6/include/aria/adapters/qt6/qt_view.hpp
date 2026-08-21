#pragma once

#include "aria/abi/export.hpp"
#include "aria/binding/view_adapter.hpp"

#include <QObject>
#include <QPointer>
#include <QWidget>

namespace aria::adapters::qt6 {

/// Wraps any QWidget* (or QObject*) as an IView.
///
/// Lifetime (L-32)
/// ---------------
/// The wrapped QObject is owned by Qt's parent-child tree, not by us.
/// We connect to `QObject::destroyed` and fire the IView destroy signal
/// from there — i.e. while this subclass's state is still valid, which is
/// what the adapter contract asks for. Relying on the `~IView` fallback
/// alone would be too late (and never happen at all for a QtView that is
/// kept alive by a cache).
///
/// `QPointer` additionally keeps `object()` safe if the widget is
/// destroyed before anyone notices.
class ARIA_QT6_API QtView final : public binding::IView {
public:
    explicit QtView(QObject* w) noexcept : w_(w) {
        if (w) {
            // No context object: the connection is owned by the sender.
            // We disconnect explicitly in the destructor so a QtView that
            // dies before its widget cannot be called back.
            destroyed_conn_ = QObject::connect(
                w, &QObject::destroyed,
                [this]() noexcept { fire_destroy_(); });
        }
    }

    ~QtView() override {
        QObject::disconnect(destroyed_conn_);
        // ~IView fires the destroy signal as a last resort; it is
        // idempotent, so a widget-first teardown does not double-fire.
    }

    [[nodiscard]] std::string_view kind() const noexcept override { return "qt6"; }

    [[nodiscard]] QObject* object() const noexcept { return w_.data(); }

    template<typename T>
    [[nodiscard]] T* as() const noexcept { return qobject_cast<T*>(w_.data()); }

private:
    QPointer<QObject>       w_;
    QMetaObject::Connection destroyed_conn_;
};

}  // namespace aria::adapters::qt6
