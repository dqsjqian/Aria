#include "ChatVm.h"

namespace showcase::chat {

// ── Publisher ────────────────────────────────────────────────────────────
ChatPublisherVm::ChatPublisherVm(aria::runtime::EventBus& bus)
    : bus_(bus),
      send(
          [this] {
              auto u = user.get();
              auto t = draft.get();
              if (!is_active().get() || u.empty() || t.empty()) return;
              bus_.publish(ChatMessage{u, t});
              draft.set(std::string{});
          },
          [this] {
              return is_active().get() && !user.get().empty() && !draft.get().empty();
          })
{}

void ChatPublisherVm::on_activate() {
    // `send`'s predicate reads `is_active`, `user` and `draft` reactively;
    // `Command<>` auto-tracks those dependencies and re-evaluates
    // can_execute on every change — no manual Effect needed here.
}

void ChatPublisherVm::on_deactivate() { bag().clear(); }

// ── Subscriber ───────────────────────────────────────────────────────────
ChatSubscriberVm::ChatSubscriberVm(aria::runtime::EventBus& bus) : bus_(bus) {}

void ChatSubscriberVm::on_activate() {
    // Subscription is attached to bag_; on_deactivate's bag().clear()
    // disconnects automatically.
    bag() += bus_.subscribe<ChatMessage>([this](const ChatMessage& m) {
        messages.push_back(std::make_shared<ChatMessage>(m));
    });
}

void ChatSubscriberVm::on_deactivate() { bag().clear(); }

// ── Composite VM: activate cascades to children ──────────────────────────────
ChatVm::ChatVm(aria::runtime::EventBus& bus)    : publisher(std::make_shared<ChatPublisherVm>(bus)),
      subscriber(std::make_shared<ChatSubscriberVm>(bus))
{
    add_child(publisher);
    add_child(subscriber);
}

}  // namespace showcase::chat
