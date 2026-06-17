#pragma once
//
// ChatVm — Tab 7: chat room
//
// Three VMs:
//   ChatPublisherVm    publishes messages onto the EventBus
//   ChatSubscriberVm   subscribes to messages and stores them in an
//                      ObservableList
//   ChatVm             composite parent that owns the two above as
//                      children; activate / deactivate cascades
//                      automatically.
//

#include "aria/aria.hpp"
#include "aria/command.hpp"
#include "aria/observable_list.hpp"
#include "aria/binding/view_model.hpp"
#include "aria/runtime/event_bus.hpp"

#include "Business/Chat/Models/ChatMessage.h"

#include <memory>
#include <string>

namespace showcase::chat {

class ChatPublisherVm : public aria::binding::ViewModel {
    aria::runtime::EventBus& bus_;
public:
    aria::Property<std::string> user{"alice"};
    aria::Property<std::string> draft{""};
    aria::Command<>             send;

    explicit ChatPublisherVm(aria::runtime::EventBus& bus);

    void on_activate() override;
    void on_deactivate() override;
};

class ChatSubscriberVm : public aria::binding::ViewModel {
    aria::runtime::EventBus& bus_;
public:
    aria::ObservableList<ChatMessage> messages;

    explicit ChatSubscriberVm(aria::runtime::EventBus& bus);

    void on_activate() override;
    void on_deactivate() override;
};

class ChatVm : public aria::binding::ViewModel {
public:
    std::shared_ptr<ChatPublisherVm>  publisher;
    std::shared_ptr<ChatSubscriberVm> subscriber;

    explicit ChatVm(aria::runtime::EventBus& bus);
};

}  // namespace showcase::chat
