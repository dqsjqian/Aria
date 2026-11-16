#include "aria/i18n/locale_context.hpp"

#include "aria/property.hpp"

#include <stdexcept>
#include <utility>

namespace aria::i18n {

struct LocaleContext::Impl {
    std::shared_ptr<Catalog> catalog;
    LocaleOptions options;
    aria::Property<std::string> language;
    aria::Property<std::uint64_t> revision{0};

    Impl(std::shared_ptr<Catalog> source, LocaleOptions selected)
        : catalog(std::move(source)), options(std::move(selected)), language(options.language) {
        if (!catalog) throw std::invalid_argument("LocaleContext requires a catalog");
        if (options.language.empty() || options.default_language.empty()) {
            throw std::invalid_argument("LocaleContext languages must not be empty");
        }
    }
};

LocaleContext::LocaleContext(std::shared_ptr<Catalog> catalog, LocaleOptions options)
    : impl_(std::make_unique<Impl>(std::move(catalog), std::move(options))) {}
LocaleContext::~LocaleContext() = default;

std::string LocaleContext::language() const { return impl_->language.get(); }
void LocaleContext::set_language(std::string language) {
    if (language.empty()) throw std::invalid_argument("LocaleContext language must not be empty");
    impl_->language.set(std::move(language));
}
aria::Subscription LocaleContext::on_language_changed(
    std::function<void(const std::string&)> callback) {
    return impl_->language.on_changed(std::move(callback));
}
std::uint64_t LocaleContext::revision() const { return impl_->revision.get(); }
aria::Subscription LocaleContext::on_catalog_changed(
    std::function<void(const std::uint64_t&)> callback) {
    return impl_->revision.on_changed(std::move(callback));
}

LookupResult LocaleContext::lookup_exact(std::string_view locale,
                                         std::string_view domain,
                                         std::string_view key) const {
    (void)revision();
    const auto selected = locale.empty() ? language() : std::string(locale);
    return impl_->catalog->lookup_exact(selected, domain, key);
}

LookupResult LocaleContext::find(std::string_view domain, std::string_view key,
                                 std::string_view locale) const {
    (void)revision();
    const auto selected = locale.empty() ? language() : std::string(locale);
    const auto& fallback = impl_->options.default_language;
    const auto& common = impl_->options.common_domain;
    const bool use_common = !common.empty() && common != domain;
    const bool use_default = selected != fallback;
    const auto query = [&](std::string_view requested_language,
                           std::string_view requested_domain) -> LookupResult {
        if (requested_domain.empty()) return std::optional<std::string>{};
        return impl_->catalog->lookup_exact(requested_language, requested_domain, key);
    };
    auto result = query(selected, domain);
    if (!result || result->has_value()) return result;

    if (impl_->options.fallback_order == FallbackOrder::LanguageFirst && use_common) {
        result = query(selected, common);
        if (!result || result->has_value()) return result;
    }
    if (use_default) {
        result = query(fallback, domain);
        if (!result || result->has_value()) return result;
    }
    if (impl_->options.fallback_order == FallbackOrder::DomainFirst && use_common) {
        result = query(selected, common);
        if (!result || result->has_value()) return result;
    }
    if (use_default && use_common) return query(fallback, common);
    return std::optional<std::string>{};
}

std::string LocaleContext::tr(std::string_view domain, std::string_view key,
                              std::string_view locale) const {
    auto result = find(domain, key, locale);
    if (!result) {
        const auto& error = result.error();
        throw std::runtime_error("i18n: " + error.message + " [" + error.path + ":" +
                                 std::to_string(error.offset) + "]");
    }
    if (*result) return std::move(**result);
    const auto shown_domain = domain.empty() ? std::string_view(impl_->options.common_domain) : domain;
    return "[" + std::string(shown_domain) + "/" + std::string(key) + "]";
}

std::vector<std::string> LocaleContext::available_languages() const {
    (void)revision();
    return impl_->catalog->available_languages();
}

std::expected<void, CatalogError> LocaleContext::reload() {
    auto result = impl_->catalog->reload();
    if (result) impl_->revision.set(impl_->revision.peek() + 1);
    return result;
}

}  // namespace aria::i18n
