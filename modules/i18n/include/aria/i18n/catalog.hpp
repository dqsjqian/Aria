#pragma once

#include "aria/i18n/export.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aria::i18n {

enum class CatalogErrorCode { invalid_identifier, io, parse };

struct CatalogError {
    CatalogErrorCode code;
    std::string message;
    std::string path;
    std::size_t offset = 0;  // Byte offset for parse errors.
};

using LookupResult = std::expected<std::optional<std::string>, CatalogError>;

/// Resource backend. Exact means exact: no language/domain fallback and no
/// missing-key placeholder. A missing resource is a miss; a damaged resource
/// is an error. Backend implementations document their own threading policy.
class ARIA_I18N_API Catalog {
public:
    virtual ~Catalog() = default;
    [[nodiscard]] virtual LookupResult lookup_exact(
        std::string_view locale, std::string_view domain, std::string_view key) const = 0;
    [[nodiscard]] virtual std::vector<std::string> available_languages() const = 0;
    /// Refresh resources transactionally: on failure preserve previous data.
    [[nodiscard]] virtual std::expected<void, CatalogError> reload() = 0;
};

}  // namespace aria::i18n
