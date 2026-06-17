#pragma once
//
// AppShell — the assembly root.
//
// Rules:
//   • All runtime objects (adapter / dispatcher / executor / timer / bus)
//     and all VMs are declared inside this class.
//   • Member declaration order = construction order = the reverse of
//     destruction order.
//   • Executor / Dispatcher / Scheduler are declared before the VMs
//     so that they outlive every VM and a VM destructor never reaches
//     into an already-released reference.
//

#include "App/Executors.h"

#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/adapters/qt6/qt_dispatcher.hpp"
#include "aria/async/executor.hpp"
#include "aria/binding/binding_engine.hpp"
#include "aria/binding/view_model.hpp"
#include "aria/runtime/event_bus.hpp"

#include <QObject>
#include <QWidget>

#include <memory>

// Forward declarations of every feature's VM.
namespace showcase::tipcalc      { class TipCalcVm; }
namespace showcase::unitconvert  { class UnitConvertVm; }
namespace showcase::cart         { class CartVm; }
namespace showcase::signup       { class SignupVm; }
namespace showcase::search       { class SearchVm; }
namespace showcase::login        { class LoginVm; }
namespace showcase::chat         { class ChatVm; }
namespace showcase::theme        { class ThemeVm; }
namespace showcase::wizard       { class WizardVm; }

namespace showcase::app {

class AppShell {
public:
    explicit AppShell(QObject* qt_ctx);
    ~AppShell();

    AppShell(const AppShell&) = delete;
    AppShell& operator=(const AppShell&) = delete;

    // Factories for the nine tabs.
    QWidget* build_tip_calc_tab();
    QWidget* build_unit_convert_tab();
    QWidget* build_cart_tab();
    QWidget* build_signup_tab();
    QWidget* build_search_tab();
    QWidget* build_login_tab();
    QWidget* build_chat_tab();
    QWidget* build_theme_tab();
    QWidget* build_wizard_tab();

    /// Tab index → VM associated with that tab (used to activate /
    /// deactivate the VM when tabs are switched). Returns nullptr for
    /// tabs whose VM does not derive from ViewModel.
    std::shared_ptr<aria::binding::ViewModel> vm_for_tab(int index);

private:
    // ── Runtime objects (declared first → destroyed last) ─────────────────
    std::shared_ptr<aria::adapters::qt6::QtDispatcher> qt_dispatcher;    DispatcherExec                                     ui_exec;
    DispatcherDelay                                    delay;
    aria::async::ThreadPoolExecutor                    worker;
    std::shared_ptr<aria::adapters::qt6::QtAdapter>    adapter;
    aria::binding::BindingEngine                       be;
    aria::runtime::EventBus                            bus;

    // ── ViewModels (declared last → destroyed first) ──────────────────────
    std::shared_ptr<tipcalc::TipCalcVm>         vm_tip;    std::shared_ptr<unitconvert::UnitConvertVm> vm_unit;
    std::shared_ptr<cart::CartVm>               vm_cart;
    std::shared_ptr<signup::SignupVm>           vm_signup;
    std::shared_ptr<search::SearchVm>           vm_search;
    std::shared_ptr<login::LoginVm>             vm_login;
    std::shared_ptr<chat::ChatVm>               vm_chat;
    std::shared_ptr<theme::ThemeVm>             vm_theme;
    std::shared_ptr<wizard::WizardVm>           vm_wizard;
};

}  // namespace showcase::app
