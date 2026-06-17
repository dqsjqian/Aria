#include "WizardVm.h"

#include <utility>

namespace showcase::wizard {

Step1Vm::Step1Vm(std::shared_ptr<WizardDraft> d) : draft(std::move(d)) {}
Step2Vm::Step2Vm(std::shared_ptr<WizardDraft> d) : draft(std::move(d)) {}
Step3Vm::Step3Vm(std::shared_ptr<WizardDraft> d) : draft(std::move(d)) {}

void Step3Vm::finish() {
    finishedSummary.set(
        "✓ " + draft->username.get() + " (" +
        draft->email.get() + ", " + draft->theme.get() + ") 注册完成");
}

WizardVm::WizardVm()
    : draft(std::make_shared<WizardDraft>()),
      nav(std::make_shared<aria::binding::Navigator>())
{
    step1 = std::make_shared<Step1Vm>(draft);
    step2 = std::make_shared<Step2Vm>(draft);
    step3 = std::make_shared<Step3Vm>(draft);
    nav->push(step1);
}

void WizardVm::toStep(int i) {
    auto target = (i == 1) ? std::static_pointer_cast<aria::binding::ViewModel>(step1)
                : (i == 2) ? std::static_pointer_cast<aria::binding::ViewModel>(step2)
                           : std::static_pointer_cast<aria::binding::ViewModel>(step3);
    nav->replace(target);
}

}  // namespace showcase::wizard
