#include "SearchView.h"

#include "App/UiHelpers.h"
#include "Business/Search/ViewModels/SearchVm.h"

#include "aria/adapters/qt6/qt_list_model_adapter.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QVBoxLayout>

namespace showcase::search {

using namespace showcase::ui;

QWidget* build_view(SearchVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto& s_subs = subs_attached_to(w);
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "搜索框：每次敲键盘 query 都变；debounce(300ms) 把连续变化压成"
        "最后一次；distinct_until_changed 再把相邻重复去掉。下方列表"
        "只在真正\"发起搜索\"时增加一条。"));

    auto* input = new QLineEdit;
    input->setPlaceholderText("输入关键词，停 300ms 后触发搜索…");
    lay->addWidget(input);
    be.bind_text(vm.query, view_for(input));

    auto* rawLbl = new QLabel;
    auto* debLbl = new QLabel;
    auto* disLbl = new QLabel;
    for (auto* l : {rawLbl, debLbl, disLbl}) {
        l->setStyleSheet("QLabel { font-family:monospace; font-size:11px; color:#546e7a; }");
    }
    lay->addWidget(rawLbl);
    lay->addWidget(debLbl);
    lay->addWidget(disLbl);

    auto syncRaw = [rawLbl](const std::string& s) {
        rawLbl->setText(QStringLiteral("  raw       : ") + QString::fromStdString(s));
    };
    auto syncDeb = [debLbl](const std::string& s) {
        debLbl->setText(QStringLiteral("  debounced : ") + QString::fromStdString(s));
    };
    auto syncDis = [disLbl](const std::string& s) {
        disLbl->setText(QStringLiteral("  distinct  : ") + QString::fromStdString(s));
    };
    syncRaw(vm.query.get());
    syncDeb(vm.debounced->get());
    syncDis(vm.distinct ->get());
    s_subs.push_back(vm.query    .on_changed(syncRaw));
    s_subs.push_back(vm.debounced->on_changed(syncDeb));
    s_subs.push_back(vm.distinct ->on_changed(syncDis));

    auto* history = new QLabel("已发起的搜索（distinct 命中才会增加）：");
    history->setStyleSheet("QLabel { color:#263238; margin-top:6px; }");
    lay->addWidget(history);

    auto* listView = new QListView;
    auto* model = new aria::adapters::qt6::ObservableListModel<SearchHit>(
        vm.hits,
        {{Qt::DisplayRole, "display"}},
        [](const SearchHit& h, int role) -> QVariant {
            if (role == Qt::DisplayRole) {
                return QString("  #%1   搜索:  \"%2\"")
                    .arg(h.seq)
                    .arg(QString::fromStdString(h.q));
            }
            return {};
        });
    listView->setModel(model);
    lay->addWidget(listView, 1);
    return w;
}

}  // namespace showcase::search
