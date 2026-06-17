#include "LoginView.h"

#include "App/UiHelpers.h"
#include "Business/Login/ViewModels/LoginVm.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace showcase::login {

using namespace showcase::ui;

QWidget* build_view(LoginVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "模拟登录。VM 继承 binding::ViewModel，is_active Property 在切 tab"
        "时翻转；submit() 只在 is_active 为真时真的发起请求。VM 析构时"
        "ViewModelScope.cancel() + AsyncCommand 自带的 cancel 会把挂起的"
        "协程一次清理掉 —— 生命周期全由框架基类托管。"));

    auto* form = new QFormLayout;
    auto* userEdit = new QLineEdit;
    auto* pwdEdit  = new QLineEdit;
    pwdEdit->setEchoMode(QLineEdit::Password);
    form->addRow("用户名", userEdit);
    form->addRow("密码",   pwdEdit);
    lay->addLayout(form);

    be.bind_text(vm.username, view_for(userEdit));
    be.bind_text(vm.password, view_for(pwdEdit));

    auto* btn = new QPushButton("登录");
    lay->addWidget(btn);

    auto* spinner = new QProgressBar;
    spinner->setRange(0, 0);
    spinner->setVisible(false);
    lay->addWidget(spinner);

    auto* errLbl = new QLabel;
    errLbl->setStyleSheet("QLabel { color:#b71c1c; font-weight:bold; }");
    lay->addWidget(errLbl);

    auto* welcome = make_result();
    welcome->setText("(未登录)");
    lay->addWidget(welcome);

    auto* activeLbl = make_sub();
    lay->addWidget(activeLbl);
    lay->addStretch();

    QObject::connect(btn, &QPushButton::clicked, [&vm] { vm.submit(); });

    auto refreshBtn = [btn, &vm] {
        const bool busy   = vm.login.is_executing.get();
        const bool active = vm.is_active().get();
        btn->setEnabled(active && !busy);
        btn->setText(busy ? "登录中…" : "登录");
    };
    refreshBtn();
    s_subs.push_back(vm.login.is_executing.on_changed([refreshBtn](bool){ refreshBtn(); }));
    s_subs.push_back(vm.is_active().on_changed([refreshBtn, activeLbl](bool a) {
        refreshBtn();
        activeLbl->setText(a ? "VM 状态: active ●" : "VM 状态: inactive ○");
    }));
    activeLbl->setText(vm.is_active().get() ? "VM 状态: active ●" : "VM 状态: inactive ○");

    s_subs.push_back(vm.login.is_executing.on_changed(
        [spinner](bool busy) { spinner->setVisible(busy); }));

    s_subs.push_back(vm.login.last_error_message.on_changed([errLbl](const std::string& e) {
        errLbl->setText(e.empty() ? "" : QStringLiteral("✗ ") + QString::fromStdString(e));
    }));
    s_subs.push_back(vm.login.last_result.on_changed(
        [welcome](const std::optional<LoginResult>& r) {
            if (r) welcome->setText(QString::fromStdString(r->welcome));
        }));

    return w;
}

}  // namespace showcase::login
