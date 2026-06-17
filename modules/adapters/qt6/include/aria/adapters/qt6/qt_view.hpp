#pragma once

#include "aria/abi/export.hpp"
#include "aria/binding/view_adapter.hpp"

#include <QPointer>
#include <QWidget>

namespace aria::adapters::qt6 {

/// Wraps any QWidget* (or QObject*) as an IView.
/// QPointer keeps us safe if the widget gets destroyed under our feet.
class ARIA_QT6_API QtView final : public binding::IView {
public:
    explicit QtView(QObject* w) noexcept : w_(w) {}

    [[nodiscard]] std::string_view kind() const noexcept override { return "qt6"; }

    [[nodiscard]] QObject* object() const noexcept { return w_.data(); }

    template<typename T>
    [[nodiscard]] T* as() const noexcept { return qobject_cast<T*>(w_.data()); }

private:
    QPointer<QObject> w_;
};

}  // namespace aria::adapters::qt6
