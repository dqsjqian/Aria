#pragma once

#include "aria/i18n/catalog.hpp"

#include <memory>
#include <string>

namespace aria::i18n {

/// Files: root/domain/strings.xml (default language), strings_<locale>.xml.
/// Supported UTF-8 XML subset: optional declaration/BOM, comments, <resources>,
/// and <string name="key">plain text / entities / CDATA</string>. Nested tags,
/// DTDs, processing instructions, other attributes and duplicate keys are errors.
/// XML newline normalization applies; Android backslash escapes do not.
///
/// Lookup/cache/reload are mutex-protected. This does NOT make LocaleContext or
/// reactive bindings cross-thread safe. Missing files are cached as misses until
/// reload. Reload validates every previously requested table before committing;
/// not-yet-requested domains/languages remain lazy. Errors include path/offset.
class ARIA_I18N_API XmlCatalog final : public Catalog {
public:
    explicit XmlCatalog(std::string root, std::string default_language);
    ~XmlCatalog() override;
    XmlCatalog(const XmlCatalog&) = delete;
    XmlCatalog& operator=(const XmlCatalog&) = delete;

    [[nodiscard]] LookupResult lookup_exact(
        std::string_view locale, std::string_view domain, std::string_view key) const override;
    /// Sorted union of filenames plus the default language. I/O failures throw
    /// std::runtime_error; enumeration does not claim translation completeness.
    [[nodiscard]] std::vector<std::string> available_languages() const override;
    [[nodiscard]] std::expected<void, CatalogError> reload() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aria::i18n
