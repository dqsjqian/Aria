#include "TipCalcView.h"

#include "App/UiHelpers.h"
#include "Business/TipCalc/ViewModels/TipCalcVm.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace showcase::tipcalc {

using showcase::ui::make_info;
using showcase::ui::make_result;
using showcase::ui::make_sub;
using showcase::ui::view_for;
using showcase::ui::subs_attached_to;

QWidget* build_view(TipCalcVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "小费计算器：账单、小费%、人数任一变化都会触发三个 Computed "
        "自动重算。Round up 按钮同时改两个 Property —— 用 "
        "reactive::batch 包住，下游只 flush 一次，UI 无闪烁。"));

    auto* form = new QFormLayout;

    auto* billSpin = new QDoubleSpinBox;
    billSpin->setRange(0.0, 99999.0);
    billSpin->setDecimals(2);
    billSpin->setPrefix("¥ ");
    form->addRow("账单", billSpin);

    auto* tipSlider = new QSlider(Qt::Horizontal);
    tipSlider->setRange(0, 30);
    auto* tipValueLbl = new QLabel;
    auto* tipRow = new QHBoxLayout;
    tipRow->addWidget(tipSlider);
    tipRow->addWidget(tipValueLbl);
    form->addRow("小费 %", tipRow);

    auto* peopleSpin = new QSpinBox;
    peopleSpin->setRange(1, 50);
    form->addRow("人数", peopleSpin);

    lay->addLayout(form);

    auto* tipLbl  = make_sub();
    auto* totalLbl = make_sub();
    auto* perLbl  = make_result();
    lay->addWidget(tipLbl);
    lay->addWidget(totalLbl);
    lay->addWidget(perLbl);

    auto* roundBtn = new QPushButton("Round up (bill 取整 + tip 对齐 5%)");
    lay->addWidget(roundBtn);
    lay->addStretch();

    be.bind_double(vm.bill,       view_for(billSpin));
    be.bind_int   (vm.tipPercent, view_for(tipSlider));
    be.bind_int   (vm.people,     view_for(peopleSpin));
    be.bind_command(vm.roundUp,   view_for(roundBtn));

    auto fmt = [](double x) { return QString::number(x, 'f', 2); };
    auto syncTip = [tipLbl, fmt](double v)   { tipLbl  ->setText("小费金额: ¥ " + fmt(v)); };
    auto syncTotal = [totalLbl, fmt](double v){ totalLbl->setText("总计: ¥ " + fmt(v)); };
    auto syncPer = [perLbl, fmt](double v)   { perLbl  ->setText("每人付: ¥ " + fmt(v)); };
    auto syncPct = [tipValueLbl](int p)      { tipValueLbl->setText(QString::number(p) + " %"); };

    syncTip (vm.tipAmount.get());
    syncTotal(vm.total.get());
    syncPer  (vm.perPerson.get());
    syncPct  (vm.tipPercent.get());

    s_subs.push_back(vm.tipAmount .on_changed(syncTip));
    s_subs.push_back(vm.total     .on_changed(syncTotal));
    s_subs.push_back(vm.perPerson .on_changed(syncPer));
    s_subs.push_back(vm.tipPercent.on_changed(syncPct));

    return w;
}

}  // namespace showcase::tipcalc
