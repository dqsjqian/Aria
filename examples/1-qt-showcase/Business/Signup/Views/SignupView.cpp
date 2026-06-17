#include "SignupView.h"

#include "App/UiHelpers.h"
#include "Business/Signup/ViewModels/SignupVm.h"

#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace showcase::signup {

using namespace showcase::ui;

namespace {

// Per-field error label rendering.
//
// FormField exposes:
//   - is_valid : Property<bool>
//   - error    : Property<std::string>   (single message, empty when valid)
// We render "✓" when valid and "✗ <error>" when not. This matches the
// new FormField shape — there is no longer a ValidationResult vector.
void render_field_hint(QLabel* out, bool valid, const std::string& err) {
    if (valid) {
        out->setText("✓");
        out->setStyleSheet("QLabel { color:#1b5e20; font-size:11px; }");
        return;
    }
    out->setText("✗ " + QString::fromStdString(err));
    out->setStyleSheet("QLabel { color:#b71c1c; font-size:11px; }");
}

void wire_field(std::vector<aria::Subscription>& subs,
                aria::binding::BindingEngine& be,
                aria::binding::FormField<std::string>& field,
                QLineEdit* edit,
                QLabel* hint) {
    be.bind_text(field.value, view_for(edit));

    auto sync = [hint, &field] {
        render_field_hint(hint, field.is_valid.get(), field.error.get());
    };
    sync();
    subs.push_back(field.is_valid.on_changed([sync](bool) { sync(); }));
    subs.push_back(field.error   .on_changed([sync](const std::string&) { sync(); }));
}

}  // namespace

QWidget* build_view(SignupVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "注册表单：每个字段是一个 binding::FormField<std::string>，"
        "由 binding::FormValidator 聚合。Submit 按钮的 enabled 直接绑定"
        "form.is_valid；password / confirm 一致性是 form.rule(...) 注册的"
        "跨字段规则——任意字段变更都会触发重算，无需手写 Effect。"));

    auto* form = new QFormLayout;

    auto* userEdit  = new QLineEdit; auto* userHint  = new QLabel;
    auto* emailEdit = new QLineEdit; auto* emailHint = new QLabel;
    auto* pwdEdit   = new QLineEdit; pwdEdit->setEchoMode(QLineEdit::Password);
    auto* pwdHint   = new QLabel;
    auto* confEdit  = new QLineEdit; confEdit->setEchoMode(QLineEdit::Password);
    auto* confHint  = new QLabel;

    form->addRow("用户名", userEdit);  form->addRow("", userHint);
    form->addRow("邮箱",   emailEdit); form->addRow("", emailHint);
    form->addRow("密码",   pwdEdit);   form->addRow("", pwdHint);
    form->addRow("确认",   confEdit);  form->addRow("", confHint);
    lay->addLayout(form);

    wire_field(s_subs, be, vm.username, userEdit,  userHint);
    wire_field(s_subs, be, vm.email,    emailEdit, emailHint);
    wire_field(s_subs, be, vm.password, pwdEdit,   pwdHint);
    wire_field(s_subs, be, vm.confirm,  confEdit,  confHint);

    // Form-level error banner: surfaces cross-field rule failures
    // (e.g. "passwords do not match") that have no single-field owner.
    auto* formError = new QLabel;
    formError->setStyleSheet("QLabel { color:#b71c1c; font-size:11px; }");
    formError->setWordWrap(true);
    lay->addWidget(formError);
    auto syncFormError = [formError](const std::string& msg) {
        formError->setText(msg.empty() ? QString{} : QString::fromStdString("⚠ " + msg));
        formError->setVisible(!msg.empty());
    };
    syncFormError(vm.form.first_error.get());
    s_subs.push_back(vm.form.first_error.on_changed(syncFormError));

    auto* submit = new QPushButton("注册");
    submit->setStyleSheet(
        "QPushButton:enabled  { background:#2e7d32; color:white; padding:8px; }"
        "QPushButton:disabled { background:#bdbdbd; color:#616161; padding:8px; }");
    lay->addWidget(submit);

    be.bind_command(vm.submit, view_for(submit));
    // Submit's enabled state is now driven directly by form.is_valid —
    // the form aggregates per-field validators AND cross-field rules,
    // so this single Property is the canonical "ready to submit" gate.
    auto syncEnabled = [submit](bool ok) { submit->setEnabled(ok); };
    syncEnabled(vm.form.is_valid.get());
    s_subs.push_back(vm.form.is_valid.on_changed(syncEnabled));

    auto* summary = make_result();
    summary->setText("(未注册)");
    lay->addWidget(summary);
    be.bind_text_oneway(vm.submittedSummary, view_for(summary));

    lay->addStretch();
    return w;
}

}  // namespace showcase::signup
