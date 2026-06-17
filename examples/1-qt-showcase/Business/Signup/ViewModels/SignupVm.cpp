#include "SignupVm.h"

#include <cctype>

namespace showcase::signup {

SignupVm::SignupVm()
    : submit([this] {
          submittedSummary.set(
              "✓ " + username.value.get() +
              " (" + email.value.get() + ") 已注册");
      })
{
    // ── Per-field rules ─────────────────────────────────────────────
    username
        .must([](const std::string& s) { return s.size() >= 3; },
              "用户名至少 3 个字符")
        .must([](const std::string& s) { return s.size() <= 16; },
              "用户名不超过 16 个字符");

    email
        .required("邮箱必填")
        .must([](const std::string& s) {
            return s.find('@') != std::string::npos
                && s.find('.') != std::string::npos;
        }, "邮箱格式不正确");

    password
        .min_length(6, "密码至少 6 位")
        .must([](const std::string& s) {
            for (char c : s) {
                if (std::isdigit(static_cast<unsigned char>(c))) return true;
            }
            return false;
        }, "密码需包含至少一位数字");

    // confirm has no per-field rule of its own — the password-match
    // check is a cross-field rule on the FormValidator below, which
    // re-evaluates whenever ANY tracked field's value changes.

    // ── Aggregate ────────────────────────────────────────────────────
    form.track(username);
    form.track(email);
    form.track(password);
    form.track(confirm);

    form.rule(
        [this] {
            return password.value.get() == confirm.value.get();
        },
        "两次密码不一致");
}

}  // namespace showcase::signup
