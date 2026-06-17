// ────────────────────────────────────────────────────────────────────────────────
//  aria — Qt6 Showcase (example 1)
//
//  Nine real-world MVVM scenarios:
//    1  Tip calculator     ViewModel + Computed + Command + batch + Effect
//    2  Unit converter     Computed conditional dependencies
//    3  Shopping cart      ViewModel + ObservableList + ItemChanged
//    4  Signup form        Validator rule chain + bind_enabled
//    5  Search box         debounce / distinct_until_changed
//    6  Simulated login    ViewModel + ViewModelScope + AsyncCommand
//    7  Chat room          ViewModel + cascading add_child lifecycle + EventBus
//    8  Theme switcher     Container (DI) + interface swap
//    9  Signup wizard      Navigator + multi-ViewModel coordination
//
//  Vertical layering by business feature:
//    App/                   Assembly layer + UiHelpers + Executors
//    Business/<Feature>/
//      Models/              Data types unique to the feature (if any)
//      ViewModels/          VMs, mostly inheriting from binding::ViewModel
//      Views/               Qt UI builder functions
//    main.cpp               QApplication + AppShell + QTabWidget + tab lifecycle
//
//  Lifecycle: switching tabs runs prev.deactivate() + cur.activate(),
//  applied only to VMs that derive from ViewModel (AppShell.vm_for_tab
//  decides which tabs participate).
// ────────────────────────────────────────────────────────────────────────────────
#include "App/AppShell.h"
#include "aria/binding/view_model.hpp"

#include <QApplication>
#include <QMainWindow>
#include <QTabWidget>
#include <QTimer>

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    showcase::app::AppShell shell{&app};

    QMainWindow win;
    win.setWindowTitle("Aria — Qt6 Showcase");
    win.resize(880, 680);

    auto* tabs = new QTabWidget;
    tabs->addTab(shell.build_tip_calc_tab(),     "小费计算器");
    tabs->addTab(shell.build_unit_convert_tab(), "单位换算");
    tabs->addTab(shell.build_cart_tab(),         "购物车");
    tabs->addTab(shell.build_signup_tab(),       "注册表单");
    tabs->addTab(shell.build_search_tab(),       "搜索框");
    tabs->addTab(shell.build_login_tab(),        "模拟登录");
    tabs->addTab(shell.build_chat_tab(),         "聊天室");
    tabs->addTab(shell.build_theme_tab(),        "主题切换");
    tabs->addTab(shell.build_wizard_tab(),       "注册向导");
    win.setCentralWidget(tabs);

    // VM lifecycle follows the active tab.
    if (auto vm = shell.vm_for_tab(0)) vm->activate();
    int last_index = 0;
    QObject::connect(tabs, &QTabWidget::currentChanged,
                     [&shell, &last_index](int idx) {
        if (auto prev = shell.vm_for_tab(last_index)) prev->deactivate();
        if (auto cur  = shell.vm_for_tab(idx))        cur->activate();
        last_index = idx;
    });

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)  // getenv safe for fixed env-var probe
#endif
    if (std::getenv("ARIA_PROBE")) {
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        QTimer::singleShot(250, &app, [tabs, &app] {
            for (int i = 0; i < tabs->count(); ++i) tabs->setCurrentIndex(i);
            std::cout << "[aria] probe: all " << tabs->count()
                      << " tabs instantiated successfully\n";
            app.quit();
        });
    }

    QObject::connect(&app, &QApplication::aboutToQuit, [&shell, &last_index] {
        if (auto cur = shell.vm_for_tab(last_index)) cur->deactivate();
    });

    win.show();
    return app.exec();
}
