#include "UiHelpers.h"

#include <memory>

namespace showcase::ui {

QLabel* make_info(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    l->setStyleSheet(
        "QLabel { color:#37474f; background:#eceff1; border:1px solid #cfd8dc;"
        " border-radius:6px; padding:8px; font-size:11px; }");
    return l;
}

QLabel* make_result(QWidget* parent) {
    auto* l = new QLabel("—", parent);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet(
        "QLabel { font-size:16px; font-weight:bold; color:#1b5e20;"
        " background:#e8f5e9; border:1px solid #a5d6a7; border-radius:6px;"
        " padding:10px; }");
    return l;
}

QLabel* make_sub(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet("QLabel { color:#546e7a; font-size:11px; }");
    return l;
}

namespace {

std::vector<std::shared_ptr<aria::adapters::qt6::QtView>>& view_keepalive() {
    static std::vector<std::shared_ptr<aria::adapters::qt6::QtView>> v;
    return v;
}

/// Subscription bag attached as a child of an owner QObject.
/// Owner destruction → this bag is destroyed via Qt's parent-child
/// chain → every Subscription is detached immediately.
class QtSubBag : public QObject {
public:
    explicit QtSubBag(QObject* parent) : QObject(parent) {}
    std::vector<aria::Subscription> subs;
};

}  // namespace

aria::adapters::qt6::QtView& view_for(QObject* w) {
    auto v = std::make_shared<aria::adapters::qt6::QtView>(w);
    auto& ref = *v;
    view_keepalive().push_back(std::move(v));
    return ref;
}

std::vector<aria::Subscription>& subs_attached_to(QObject* owner) {
    return (new QtSubBag(owner))->subs;
}

}  // namespace showcase::ui
