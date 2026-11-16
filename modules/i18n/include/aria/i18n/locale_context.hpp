#pragma once

#include "aria/i18n/catalog.hpp"
#include "aria/subscription.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace aria::i18n {

enum class FallbackOrder {
    DomainFirst,    // domain/current, domain/default, common/current, common/default
    LanguageFirst  // domain/current, common/current, domain/default, common/default
};

struct LocaleOptions {
    std::string language = "en";
    std::string default_language = "en";
    std::string common_domain;
    FallbackOrder fallback_order = FallbackOrder::DomainFirst;
};

/// An explicitly owned application/window locale; there is no global context.
/// All methods, subscriptions and destruction follow Aria's graph-owner-thread
/// contract. For background work use an explicitly captured locale and a backend
/// with an appropriate threading contract, or translate after returning to UI.
/// Language strings are open identifiers: no implicit BCP-47 alias normalization
/// is performed. Hosts choose system-language matching and persistence policy.
class ARIA_I18N_API LocaleContext final {
public:
    explicit LocaleContext(std::shared_ptr<Catalog> catalog, LocaleOptions options = {});
    ~LocaleContext();
    LocaleContext(const LocaleContext&) = delete;
    LocaleContext& operator=(const LocaleContext&) = delete;

    [[nodiscard]] std::string language() const;  // Reactive read.
    void set_language(std::string language);
    [[nodiscard]] aria::Subscription on_language_changed(
        std::function<void(const std::string&)> callback);
    [[nodiscard]] std::uint64_t revision() const;  // Reactive resource revision.
    [[nodiscard]] aria::Subscription on_catalog_changed(
        std::function<void(const std::uint64_t&)> callback);

    /// Empty locale selects the current language. Both lookup methods track
    /// catalog revision, so Computed/Effect translations refresh after reload.
    [[nodiscard]] LookupResult find(std::string_view domain, std::string_view key,
                                    std::string_view locale = {}) const;
    [[nodiscard]] LookupResult lookup_exact(std::string_view locale,
                                            std::string_view domain,
                                            std::string_view key) const;
    /// Returns [domain/key] on miss. Resource errors throw std::runtime_error
    /// including the resource path and byte offset; use find() for typed errors.
    [[nodiscard]] std::string tr(std::string_view domain, std::string_view key,
                                  std::string_view locale = {}) const;
    [[nodiscard]] std::vector<std::string> available_languages() const;
    /// Reload this context's backend and notify reactive translations only on
    /// success. Shared backends require each context to reload to notify its graph.
    [[nodiscard]] std::expected<void, CatalogError> reload();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aria::i18n
