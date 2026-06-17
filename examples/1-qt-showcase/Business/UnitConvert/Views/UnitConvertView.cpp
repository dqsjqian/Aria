#include "UnitConvertView.h"

#include "App/UiHelpers.h"
#include "Business/UnitConvert/ViewModels/UnitConvertVm.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace showcase::unitconvert {

using namespace showcase::ui;

QWidget* build_view(UnitConvertVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "单位换算器。Computed 里用 switch 根据 category 走不同公式 —— "
        "切换类别时 Computed 每次 recompute 都重新追踪 read，自动重建"
        "依赖集。这就是 Aria 的\"条件依赖\"特性。"));

    auto* cat = new QComboBox;
    cat->addItem("温度  (°C → °F)");
    cat->addItem("长度  (m  → ft)");
    cat->addItem("重量  (kg → lb)");
    lay->addWidget(cat);

    auto* valSpin = new QDoubleSpinBox;
    valSpin->setRange(-1000.0, 1000000.0);
    valSpin->setDecimals(3);
    auto* fromLbl = new QLabel;
    auto* fromRow = new QHBoxLayout;
    fromRow->addWidget(new QLabel("输入: "));
    fromRow->addWidget(valSpin);
    fromRow->addWidget(fromLbl);
    lay->addLayout(fromRow);

    auto* out = make_result();
    lay->addWidget(out);
    lay->addStretch();

    be.bind_double(vm.value, view_for(valSpin));

    QObject::connect(cat, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [&vm](int idx) { vm.category.set(static_cast<Category>(idx)); });

    auto syncResult = [out, &vm] {
        out->setText(QString("= %1 %2")
            .arg(vm.converted.get(), 0, 'f', 3)
            .arg(QString::fromStdString(vm.toLabel.get())));
    };
    auto syncFrom = [fromLbl](const std::string& s) { fromLbl->setText(QString::fromStdString(s)); };

    syncFrom(vm.fromLabel.get());
    syncResult();
    s_subs.push_back(vm.fromLabel .on_changed(syncFrom));
    s_subs.push_back(vm.converted .on_changed([syncResult](double){ syncResult(); }));
    s_subs.push_back(vm.toLabel   .on_changed([syncResult](const std::string&){ syncResult(); }));

    return w;
}

}  // namespace showcase::unitconvert
