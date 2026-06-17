#include "ChatView.h"

#include "App/UiHelpers.h"
#include "Business/Chat/ViewModels/ChatVm.h"

#include "aria/adapters/qt6/qt_list_model_adapter.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QVBoxLayout>

namespace showcase::chat {

using namespace showcase::ui;

QWidget* build_view(ChatVm& vm, aria::binding::BindingEngine& be) {
    auto* w = new QWidget;
    auto* lay = new QVBoxLayout(w);

    lay->addWidget(make_info(
        "组合 VM：ChatVm 是 parent，Publisher/Subscriber 是 children。"
        "离开 tab 时 parent.deactivate() 级联两个子 VM 的 on_deactivate —— "
        "Subscriber 停止订阅 bus，Publisher 的 CanExecute 不再更新。"
        "这就是 ViewModel 树状生命周期。"));

    auto& pub = *vm.publisher;
    auto& sub = *vm.subscriber;

    lay->addWidget(new QLabel("<b>Publisher</b>"));
    auto* row = new QHBoxLayout;
    auto* userEdit = new QLineEdit; userEdit->setPlaceholderText("用户名");
    userEdit->setMaximumWidth(100);
    auto* draftEdit = new QLineEdit; draftEdit->setPlaceholderText("输入消息 …");
    auto* sendBtn = new QPushButton("发送");
    row->addWidget(userEdit);
    row->addWidget(draftEdit, 1);
    row->addWidget(sendBtn);
    lay->addLayout(row);

    be.bind_text   (pub.user,  view_for(userEdit));
    be.bind_text   (pub.draft, view_for(draftEdit));
    be.bind_command(pub.send,  view_for(sendBtn));

    lay->addWidget(new QLabel("<b>Subscriber</b>"));
    auto* listView = new QListView;
    auto* model = new aria::adapters::qt6::ObservableListModel<ChatMessage>(
        sub.messages,
        {{Qt::DisplayRole, "display"}},
        [](const ChatMessage& m, int role) -> QVariant {
            if (role == Qt::DisplayRole) {
                return QString("  %1:  %2")
                    .arg(QString::fromStdString(m.user))
                    .arg(QString::fromStdString(m.text));
            }
            return {};
        });
    listView->setModel(model);
    lay->addWidget(listView, 1);
    return w;
}

}  // namespace showcase::chat
