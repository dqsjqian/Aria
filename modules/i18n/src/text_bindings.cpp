#include "aria/i18n/text_bindings.hpp"

#include <stdexcept>
#include <utility>

namespace aria::i18n {

aria::Subscription bind_text(std::shared_ptr<LocaleContext> context, std::string domain,
                             aria::Property<std::string>& target, std::string key) {
    if (!context) throw std::invalid_argument("bind_text requires a locale context");
    aria::reactive::detail::NodeHandle alive{&target};
    return aria::reactive::Effect{
        [context = std::move(context), domain = std::move(domain),
         key = std::move(key), alive = std::move(alive), &target] {
            if (!alive) return;
            target.set(context->tr(domain, key));
        }}.into_subscription();
}

aria::Subscription bind_localized(std::shared_ptr<LocaleContext> context,
                                  std::function<void()> callback) {
    if (!context || !callback) {
        throw std::invalid_argument("bind_localized requires a context and callback");
    }
    return aria::reactive::Effect{
        [context = std::move(context), callback = std::move(callback)] {
            (void)context->language();
            (void)context->revision();
            callback();
        }}.into_subscription();
}

struct TextBindings::Impl {
    std::shared_ptr<LocaleContext> context;
    std::string domain;
    aria::SubscriptionBag subscriptions;
};

TextBindings::TextBindings(std::shared_ptr<LocaleContext> context, std::string domain)
    : impl_(std::make_unique<Impl>(std::move(context), std::move(domain), aria::SubscriptionBag{})) {
    if (!impl_->context) throw std::invalid_argument("TextBindings requires a locale context");
}
TextBindings::~TextBindings() = default;
void TextBindings::text(aria::Property<std::string>& target, std::string key) {
    impl_->subscriptions += bind_text(impl_->context, impl_->domain, target, std::move(key));
}
void TextBindings::localize(std::function<void()> callback) {
    impl_->subscriptions += bind_localized(impl_->context, std::move(callback));
}
void TextBindings::clear() { impl_->subscriptions.clear(); }

}  // namespace aria::i18n
