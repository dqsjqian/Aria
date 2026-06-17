#include "CartView.h"

#include "App/UiHelpers.h"
#include "Business/Cart/ViewModels/CartVm.h"

#include "aria/adapters/qt6/qt_list_model_adapter.hpp"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QVBoxLayout>

namespace showcase::cart {

using namespace showcase::ui;

QWidget* build_view(CartVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "ObservableList<CartItem> + ObservableListModel 直接接 QListView。"
        "每个 item 的 qty 改变冒泡成 ItemChanged，model 只刷那一行。"
        "汇总用 Effect/on_any_change 做 incremental sum，全写在 VM 里。"));

    // Add-item form
    auto* form = new QFormLayout;
    auto* nameEdit = new QLineEdit;
    auto* priceSpin = new QDoubleSpinBox;
    priceSpin->setRange(0.01, 9999.0);
    priceSpin->setDecimals(2);
    priceSpin->setPrefix("¥ ");
    auto* addBtn = new QPushButton("加入购物车");
    form->addRow("商品", nameEdit);
    form->addRow("单价", priceSpin);
    form->addRow("",     addBtn);
    lay->addLayout(form);

    be.bind_text  (vm.draftName,  view_for(nameEdit));
    be.bind_double(vm.draftPrice, view_for(priceSpin));
    be.bind_command(vm.addItem,   view_for(addBtn));

    // List
    auto* listView = new QListView;
    listView->setAlternatingRowColors(true);
    auto* model = new aria::adapters::qt6::ObservableListModel<CartItem>(
        vm.items,
        {{Qt::DisplayRole, "display"}},
        [](const CartItem& it, int role) -> QVariant {
            if (role == Qt::DisplayRole) {
                return QString("  %1  ×  %2     ¥ %3  →  ¥ %4")
                    .arg(QString::fromStdString(it.name()))
                    .arg(it.qty_value())
                    .arg(it.price(),    0, 'f', 2)
                    .arg(it.subtotal(), 0, 'f', 2);
            }
            return {};
        });
    listView->setModel(model);
    lay->addWidget(listView, 1);

    // Action buttons
    auto* ops = new QHBoxLayout;
    auto* plusBtn  = new QPushButton("数量 +1");
    auto* minusBtn = new QPushButton("数量 -1");
    auto* delBtn   = new QPushButton("删除选中");
    ops->addWidget(plusBtn);
    ops->addWidget(minusBtn);
    ops->addWidget(delBtn);
    lay->addLayout(ops);

    auto currentItem = [listView, &vm]() -> std::shared_ptr<CartItem> {
        auto idx = listView->currentIndex();
        if (!idx.isValid()) return nullptr;
        auto snap = vm.items.snapshot();
        const auto row = idx.row();
        if (row < 0 || static_cast<std::size_t>(row) >= snap.size()) return nullptr;
        return snap[static_cast<std::size_t>(row)];
    };

    QObject::connect(plusBtn, &QPushButton::clicked, [currentItem] {
        if (auto it = currentItem()) it->qty().set(it->qty_value() + 1);
    });
    QObject::connect(minusBtn, &QPushButton::clicked, [currentItem] {
        if (auto it = currentItem()) {
            const int n = it->qty_value();
            if (n > 1) it->qty().set(n - 1);
        }
    });
    QObject::connect(delBtn, &QPushButton::clicked, [listView, &vm] {
        auto idx = listView->currentIndex();
        if (idx.isValid()) vm.items.remove_at(static_cast<std::size_t>(idx.row()));
    });

    // Summary area
    auto* countLbl    = make_sub();
    auto* subtotalLbl = make_sub();
    auto* taxLbl      = make_sub();
    auto* totalLbl    = make_result();
    lay->addWidget(countLbl);
    lay->addWidget(subtotalLbl);
    lay->addWidget(taxLbl);
    lay->addWidget(totalLbl);

    auto fmt = [](double v) { return QString::number(v, 'f', 2); };
    auto sC = [countLbl](int n)             { countLbl   ->setText(QString("共 %1 件商品").arg(n)); };
    auto sS = [subtotalLbl, fmt](double v)  { subtotalLbl->setText("小计: ¥ " + fmt(v)); };
    auto sT = [taxLbl, fmt](double v)       { taxLbl     ->setText("税 (8%): ¥ " + fmt(v)); };
    auto sG = [totalLbl, fmt](double v)     { totalLbl   ->setText("总计: ¥ " + fmt(v)); };

    sC(vm.itemCount.get());
    sS(vm.subtotal .get());
    sT(vm.tax      .get());
    sG(vm.total    .get());

    s_subs.push_back(vm.itemCount.on_changed(sC));
    s_subs.push_back(vm.subtotal .on_changed(sS));
    s_subs.push_back(vm.tax      .on_changed(sT));
    s_subs.push_back(vm.total    .on_changed(sG));

    return w;
}

}  // namespace showcase::cart
