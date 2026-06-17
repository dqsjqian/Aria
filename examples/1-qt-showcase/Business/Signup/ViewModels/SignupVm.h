#pragma once
//
// SignupVm — Tab 4: signup form (FormField + FormValidator showcase).
//
// Each field is a `binding::FormField<std::string>`, which bundles a
// Property<value>, a Validator, plus is_valid / error / touched / dirty.
// A single `binding::FormValidator` aggregates them and adds the
// cross-field "passwords match" rule. Submit's enabled state is driven
// by `form.is_valid` directly — no manual Computed, no Effect hack.
//

#include "aria/aria.hpp"
#include "aria/binding/form.hpp"
#include "aria/command.hpp"

#include <string>

namespace showcase::signup {

class SignupVm {
public:
    SignupVm();

    aria::binding::FormField<std::string> username{"username", ""};
    aria::binding::FormField<std::string> email{"email", ""};
    aria::binding::FormField<std::string> password{"password", ""};
    aria::binding::FormField<std::string> confirm{"confirm", ""};

    aria::binding::FormValidator form;

    aria::Property<std::string> submittedSummary{""};

    aria::Command<> submit;
};

}  // namespace showcase::signup
