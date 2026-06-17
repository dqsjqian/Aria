#pragma once

// ============================================================================
//  aria/validation_key.hpp
// ----------------------------------------------------------------------------
//  `ValidationKey` and `Severity` -- the locator + severity primitives
//  used by the unified `aria::Error` model (see <aria/error.hpp>).
//
//  Per docs/error-model.md every validator-produced error is just an
//  `aria::Error` with `kind == ErrorKind::Validation` and a populated
//  `key`. There is **no** independent `ValidationError` struct -- the
//  unification is the whole point of the unified error taxonomy.
//
//  Why split this out from `error.hpp`?
//    * `ValidationKey` is also handy in test code and pure validation
//      helpers that do not want to drag in the full Error type.
//    * Keeps `error.hpp` free of any forward-declaration tricks.
//    * The two together form one "locator" toolbox; users that need
//      both should `#include <aria/error.hpp>`, which transitively
//      pulls this header in.
//
//  Per docs/api-style.md S-1 these names live in `aria::`.
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <utility>

namespace aria {

// ---------------------------------------------------------------------------
//  Severity
// ---------------------------------------------------------------------------

/// Severity of a single error / warning. Carried on `aria::Error`;
/// also pre-declared here so consumers that only need
/// `ValidationKey + Severity` (without the whole `Error` machinery)
/// can stay lightweight.
enum class Severity : std::uint8_t {
    Error   = 0,
    Warning = 1,
};

// ---------------------------------------------------------------------------
//  ValidationKey
// ---------------------------------------------------------------------------

/// `(field_path, rule_id)` locator for a validation message.
struct ValidationKey {
    std::string field_path;   ///< e.g. "user.email"; empty = form-level
    std::string rule_id;      ///< e.g. "required"; empty = anonymous

    [[nodiscard]] bool empty() const noexcept {
        return field_path.empty() && rule_id.empty();
    }

    /// Stable string view of the form `"<field_path>#<rule_id>"`.
    [[nodiscard]] std::string to_string() const {
        std::string out;
        out.reserve(field_path.size() + rule_id.size() + 1);
        out.append(field_path);
        out.push_back('#');
        out.append(rule_id);
        return out;
    }
};

inline bool operator==(const ValidationKey& a, const ValidationKey& b) noexcept {
    return a.field_path == b.field_path && a.rule_id == b.rule_id;
}
inline bool operator!=(const ValidationKey& a, const ValidationKey& b) noexcept {
    return !(a == b);
}

inline std::ostream& operator<<(std::ostream& os, const ValidationKey& k) {
    return os << k.to_string();
}

// ---------------------------------------------------------------------------
//  Sugar: `field("...") / rule_id("...")` builder
// ---------------------------------------------------------------------------

namespace validation_dsl {

struct FieldPart { std::string path; };
struct RulePart  { std::string id; };

}  // namespace validation_dsl

[[nodiscard]] inline validation_dsl::FieldPart field(std::string path) noexcept {
    return validation_dsl::FieldPart{std::move(path)};
}
[[nodiscard]] inline validation_dsl::RulePart rule_id(std::string id) noexcept {
    return validation_dsl::RulePart{std::move(id)};
}

[[nodiscard]] inline ValidationKey
operator/(validation_dsl::FieldPart f, validation_dsl::RulePart r) {
    return ValidationKey{std::move(f.path), std::move(r.id)};
}

}  // namespace aria

// ---------------------------------------------------------------------------
//  std::hash specialisation -- so ValidationKey can be a map key.
// ---------------------------------------------------------------------------
namespace std {

template<>
struct hash<::aria::ValidationKey> {
    [[nodiscard]] std::size_t operator()(const ::aria::ValidationKey& k) const noexcept {
        std::size_t h = std::hash<std::string>{}(k.field_path);
        h ^= std::hash<std::string>{}(k.rule_id) + 0x9e3779b97f4a7c15ULL
             + (h << 6) + (h >> 2);
        return h;
    }
};

}  // namespace std
