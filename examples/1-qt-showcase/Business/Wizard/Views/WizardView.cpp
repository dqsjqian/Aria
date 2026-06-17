#include "WizardView.h"

#include "App/UiHelpers.h"
#include "Business/Wizard/ViewModels/WizardVm.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace showcase::wizard {

using namespace showcase::ui;

namespace {

QWidget* build_step3_page(Step3Vm& vm) {
    auto* w = new QWidget;
    auto& subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);
    lay->addWidget(new QLabel("<h3>第 3 步 — 确认</h3>"));

    auto* summary = new QLabel;
    summary->setStyleSheet("QLabel { font-family:monospace; background:#f5f5f5;"
                           " padding:10px; border-radius:6px; }");
    summary->setText(QString(
        "用户名: %1\n邮箱: %2\n主题: %3")
        .arg(QString::fromStdString(vm.draft->username.get()))
        .arg(QString::fromStdString(vm.draft->email.get()))
        .arg(QString::fromStdString(vm.draft->theme.get())));
    lay->addWidget(summary);

    auto* finishBtn = new QPushButton("完成注册");
    lay->addWidget(finishBtn);
    auto* result = make_result();
    result->setText("(未完成)");
    lay->addWidget(result);
    lay->addStretch();

    QObject::connect(finishBtn, &QPushButton::clicked, [&vm] { vm.finish(); });
    auto sync = [result](const std::string& s) {
        if (!s.empty()) result->setText(QString::fromStdString(s));
    };
    subs.push_back(vm.finishedSummary.on_changed(sync));
    return w;
}

QWidget* build_step1_page(Step1Vm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    lay->addWidget(new QLabel("<h3>第 1 步 — 账户</h3>"));
    auto* form = new QFormLayout;
    auto* user = new QLineEdit;
    auto* mail = new QLineEdit;
    form->addRow("用户名", user);
    form->addRow("邮箱",   mail);
    lay->addLayout(form);
    lay->addStretch();
    be.bind_text(vm.draft->username, view_for(user));
    be.bind_text(vm.draft->email,    view_for(mail));
    return w;
}

QWidget* build_step2_page(Step2Vm& vm) {
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);
    lay->addWidget(new QLabel("<h3>第 2 步 — 偏好</h3>"));
    auto* form = new QFormLayout;
    auto* themeBox = new QComboBox;
    themeBox->addItems({"Light", "Dark", "Solarized"});
    themeBox->setCurrentText(QString::fromStdString(vm.draft->theme.get()));
    form->addRow("主题", themeBox);
    lay->addLayout(form);
    lay->addStretch();

    QObject::connect(themeBox, &QComboBox::currentTextChanged,
                     [&vm](const QString& s) { vm.draft->theme.set(s.toStdString()); });
    return w;
}

}  // namespace

QWidget* build_view(WizardVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "三步注册向导。每一步是一个 ViewModel（派生自 binding::ViewModel）；"
        "Navigator 负责 push / replace / pop，同时自动 activate/deactivate。"
        "三个 step 共享同一个 WizardDraft（shared_ptr），用完在第 3 步读出。"));

    auto* bar = new QHBoxLayout;
    auto* b1 = new QPushButton("1 · 账户");
    auto* b2 = new QPushButton("2 · 偏好");
    auto* b3 = new QPushButton("3 · 确认");
    bar->addWidget(b1); bar->addWidget(b2); bar->addWidget(b3);
    lay->addLayout(bar);

    auto* stack = new QStackedWidget;
    auto* page1 = build_step1_page(*vm.step1, be);
    auto* page2 = build_step2_page(*vm.step2);
    auto* page3 = build_step3_page(*vm.step3);
    stack->addWidget(page1);
    stack->addWidget(page2);
    stack->addWidget(page3);
    lay->addWidget(stack, 1);

    auto* depthLbl = new QLabel;
    depthLbl->setStyleSheet("QLabel { color:#546e7a; font-size:11px; }");
    lay->addWidget(depthLbl);

    QObject::connect(b1, &QPushButton::clicked, [&vm, stack] { vm.toStep(1); stack->setCurrentIndex(0); });
    QObject::connect(b2, &QPushButton::clicked, [&vm, stack] { vm.toStep(2); stack->setCurrentIndex(1); });
    QObject::connect(b3, &QPushButton::clicked,
                     [&vm, stack, page3, &be]() mutable {
        auto* newPage = build_step3_page(*vm.step3);
        int idx = stack->indexOf(page3);
        if (idx >= 0) { stack->removeWidget(page3); page3->deleteLater(); }
        stack->insertWidget(2, newPage);
        page3 = newPage;
        vm.toStep(3);
        stack->setCurrentIndex(2);
        (void)be;
    });

    s_subs.push_back(vm.nav->current.on_changed(
        [depthLbl, &vm](const std::shared_ptr<aria::binding::ViewModel>&) {
            depthLbl->setText(QString("Navigator current depth = %1")
                              .arg(vm.nav->depth.get()));
        }));
    depthLbl->setText(QString("Navigator current depth = %1").arg(vm.nav->depth.get()));
    return w;
}

}  // namespace showcase::wizard
