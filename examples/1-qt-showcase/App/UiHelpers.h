#pragma once
//
// UiHelpers.h — small UI + subscription helpers shared by every View.
//

#include "aria/adapters/qt6/qt_view.hpp"
#include "aria/subscription.hpp"

#include <QLabel>
#include <QObject>
#include <QString>
#include <QWidget>

#include <vector>

namespace showcase::ui {

// ── Visual styling ────────────────────────────────────────────────────────
QLabel* make_info(const QString& text, QWidget* parent = nullptr);
QLabel* make_result(QWidget* parent = nullptr);
QLabel* make_sub(const QString& text = "", QWidget* parent = nullptr);

// ── QtView / Subscription lifecycle ───────────────────────────────────────
aria::adapters::qt6::QtView& view_for(QObject* w);
std::vector<aria::Subscription>& subs_attached_to(QObject* owner);

}  // namespace showcase::ui
