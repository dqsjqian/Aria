#pragma once
//
// WizardVm — Tab 9: 3-step signup wizard (Navigator)
//

#include "aria/aria.hpp"
#include "aria/binding/navigation.hpp"
#include "aria/binding/view_model.hpp"

#include <memory>
#include <string>

namespace showcase::wizard {

struct WizardDraft {
    aria::Property<std::string> username{""};
    aria::Property<std::string> email{""};
    aria::Property<std::string> theme{"Light"};
};

class Step1Vm : public aria::binding::ViewModel {
public:
    std::shared_ptr<WizardDraft> draft;
    explicit Step1Vm(std::shared_ptr<WizardDraft> d);
};

class Step2Vm : public aria::binding::ViewModel {
public:
    std::shared_ptr<WizardDraft> draft;
    explicit Step2Vm(std::shared_ptr<WizardDraft> d);
};

class Step3Vm : public aria::binding::ViewModel {
public:
    std::shared_ptr<WizardDraft> draft;
    aria::Property<std::string>  finishedSummary{""};

    explicit Step3Vm(std::shared_ptr<WizardDraft> d);
    void finish();
};

class WizardVm {
public:
    WizardVm();

    std::shared_ptr<WizardDraft>              draft;
    std::shared_ptr<aria::binding::Navigator> nav;

    std::shared_ptr<Step1Vm> step1;
    std::shared_ptr<Step2Vm> step2;
    std::shared_ptr<Step3Vm> step3;

    void toStep(int i);
};

}  // namespace showcase::wizard
