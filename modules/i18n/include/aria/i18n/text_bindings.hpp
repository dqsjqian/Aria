#pragma once

#include "aria/i18n/locale_context.hpp"
#include "aria/property.hpp"

#include <functional>
#include <memory>
#include <string>

namespace aria::i18n {

/// Eager initial translation, then refresh on locale/catalog changes. The
/// subscription keeps the context alive and checks target Property lifetime.
/// Release on the graph thread. No ViewModel inheritance is required.
[[nodiscard]] ARIA_I18N_API aria::Subscription bind_text(
    std::shared_ptr<LocaleContext> context, std::string domain,
    aria::Property<std::string>& target, std::string key);

/// Run once now, then on locale/catalog changes AND any Property/Computed read
/// by callback. Captured objects must outlive the subscription; arbitrary
/// callback captures cannot be lifetime-checked. All work is graph-thread-only.
[[nodiscard]] ARIA_I18N_API aria::Subscription bind_localized(
    std::shared_ptr<LocaleContext> context, std::function<void()> callback);

/// Optional composition helper owning several subscriptions. Declare after the
/// objects captured by localize callbacks, or clear before destroying them.
class ARIA_I18N_API TextBindings final {
public:
    explicit TextBindings(std::shared_ptr<LocaleContext> context, std::string domain);
    ~TextBindings();
    TextBindings(const TextBindings&) = delete;
    TextBindings& operator=(const TextBindings&) = delete;

    void text(aria::Property<std::string>& target, std::string key);
    void localize(std::function<void()> callback);
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aria::i18n
