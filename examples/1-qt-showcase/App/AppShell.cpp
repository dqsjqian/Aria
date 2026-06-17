#include "AppShell.h"

#include "Business/Cart/ViewModels/CartVm.h"
#include "Business/Chat/ViewModels/ChatVm.h"
#include "Business/Login/ViewModels/LoginVm.h"
#include "Business/Search/ViewModels/SearchVm.h"
#include "Business/Signup/ViewModels/SignupVm.h"
#include "Business/Theme/ViewModels/ThemeVm.h"
#include "Business/TipCalc/ViewModels/TipCalcVm.h"
#include "Business/UnitConvert/ViewModels/UnitConvertVm.h"
#include "Business/Wizard/ViewModels/WizardVm.h"

#include "Business/Cart/Views/CartView.h"
#include "Business/Chat/Views/ChatView.h"
#include "Business/Login/Views/LoginView.h"
#include "Business/Search/Views/SearchView.h"
#include "Business/Signup/Views/SignupView.h"
#include "Business/Theme/Views/ThemeView.h"
#include "Business/TipCalc/Views/TipCalcView.h"
#include "Business/UnitConvert/Views/UnitConvertView.h"
#include "Business/Wizard/Views/WizardView.h"

namespace showcase::app {

AppShell::AppShell(QObject* qt_ctx)
    : qt_dispatcher(std::make_shared<aria::adapters::qt6::QtDispatcher>(qt_ctx)),
      ui_exec(*qt_dispatcher),
      delay(*qt_dispatcher),
      worker(2),
      adapter(std::make_shared<aria::adapters::qt6::QtAdapter>()),
      be(adapter)
{
    vm_tip      = std::make_shared<tipcalc::TipCalcVm>();
    vm_unit     = std::make_shared<unitconvert::UnitConvertVm>();
    vm_cart     = std::make_shared<cart::CartVm>();
    vm_signup   = std::make_shared<signup::SignupVm>();
    vm_search   = std::make_shared<search::SearchVm>(delay);
    vm_login    = std::make_shared<login::LoginVm>(ui_exec, worker);
    vm_chat     = std::make_shared<chat::ChatVm>(bus);
    vm_theme    = std::make_shared<theme::ThemeVm>();
    vm_wizard   = std::make_shared<wizard::WizardVm>();
}

AppShell::~AppShell() = default;

QWidget* AppShell::build_tip_calc_tab()     { return tipcalc     ::build_view(*vm_tip,    be); }
QWidget* AppShell::build_unit_convert_tab() { return unitconvert ::build_view(*vm_unit,   be); }
QWidget* AppShell::build_cart_tab()         { return cart        ::build_view(*vm_cart,   be); }
QWidget* AppShell::build_signup_tab()       { return signup      ::build_view(*vm_signup, be); }
QWidget* AppShell::build_search_tab()       { return search      ::build_view(*vm_search, be); }
QWidget* AppShell::build_login_tab()        { return login       ::build_view(*vm_login,  be); }
QWidget* AppShell::build_chat_tab()         { return chat        ::build_view(*vm_chat,   be); }
QWidget* AppShell::build_theme_tab()        { return theme       ::build_view(*vm_theme,  be); }
QWidget* AppShell::build_wizard_tab()       { return wizard      ::build_view(*vm_wizard, be); }

// Tab indices match the addTab order in main.cpp.
std::shared_ptr<aria::binding::ViewModel> AppShell::vm_for_tab(int index) {
    switch (index) {
        case 0: return vm_tip;      // TipCalc (ViewModel)
        case 1: return nullptr;     // UnitConvert (simple VM, no lifecycle)
        case 2: return vm_cart;     // Cart (ViewModel)
        case 3: return nullptr;     // Signup (simple VM)
        case 4: return nullptr;     // Search
        case 5: return vm_login;    // Login (ViewModel + ViewModelScope)
        case 6: return vm_chat;     // Chat (ViewModel + cascaded children)
        case 7: return nullptr;     // Theme
        case 8: return nullptr;     // Wizard (drives its own internal Navigator)
    }
    return nullptr;
}

}  // namespace showcase::app
