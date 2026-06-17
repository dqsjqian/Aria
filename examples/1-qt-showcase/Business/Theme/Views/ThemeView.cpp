#include "ThemeView.h"

#include "App/UiHelpers.h"
#include "Business/Theme/ViewModels/ThemeVm.h"

#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

namespace showcase::theme {

using namespace showcase::ui;

namespace {

void apply_theme(QFrame* card, QLabel* title, QLabel* body, const ITheme& th) {
    auto s = [](std::string_view v) {
        return QString::fromUtf8(v.data(), static_cast<int>(v.size()));
    };
    card->setStyleSheet(QString(
        "QFrame { background:%1; border:1px solid %2; border-radius:10px; }")
        .arg(s(th.cardBg())).arg(s(th.cardBorder())));
    const QString fg = s(th.cardFg());
    title->setStyleSheet(QString("QLabel { color:%1; font-size:18px; font-weight:bold; }").arg(fg));
    body ->setStyleSheet(QString("QLabel { color:%1; font-size:12px; }").arg(fg));
}

}  // namespace

QWidget* build_view(ThemeVm& vm, aria::binding::BindingEngine& /*be*/) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "DI Container：三个 ITheme 实现 (Light / Dark / Solarized)。"
        "切换下拉框时 clear() + register_singleton<ITheme, XxxTheme>()，"
        "然后 resolve<ITheme>() 拿到当前实现，刷新下面的示例卡片。"
        "同一套接口、三种替换 —— 这就是替换日志实现 / 支付网关 / HTTP 后端的 pattern。"));

    auto* picker = new QComboBox;
    picker->addItems({"Light", "Dark", "Solarized"});
    lay->addWidget(picker);

    auto* card = new QFrame;
    auto* cardLay = new QVBoxLayout(card);
    auto* title = new QLabel("Aria 框架");
    auto* body = new QLabel(
        "这是一个示例卡片 —— 切换主题，它的背景 / 字体 / 边框都会换。\n"
        "换实现的代码只有一行 (container.register_singleton<ITheme, ...>)，"
        "View 只认 ITheme 抽象。");
    body->setWordWrap(true);
    cardLay->addWidget(title);
    cardLay->addWidget(body);
    lay->addWidget(card, 1);

    apply_theme(card, title, body, *vm.theme());

    QObject::connect(picker, &QComboBox::currentTextChanged,
                     [&vm, card, title, body](const QString& s) {
        vm.pick(s.toStdString());
        apply_theme(card, title, body, *vm.theme());
    });

    s_subs.push_back(vm.currentName.on_changed(
        [card, title, body, &vm](const std::string&) {
            apply_theme(card, title, body, *vm.theme());
        }));

    return w;
}

}  // namespace showcase::theme
