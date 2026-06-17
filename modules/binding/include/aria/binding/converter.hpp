#pragma once

#include "aria/inplace_function.hpp"

#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace aria::binding {

/// Thrown by built-in `Converter::to_model` when the user-provided string
/// cannot be parsed into the target Model type. Binding-engine setters
/// route this exception through the unified callback-boundary channel
/// (category "binding.converter") and *do not* write a default-constructed
/// value into the Model — the previous text remains authoritative.
class ConversionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Bidirectional converter between Model type T and View type U.
///
/// Two parsing channels are supported on the View → Model direction:
///
///   * `try_to_model(view_value)` — returns `std::nullopt` on failure.
///     **Preferred**: lets the binding engine drop the update silently
///     without touching the Model. Built-in converters always populate
///     this field.
///
///   * `to_model(view_value)` — historical channel. Built-in converters
///     also populate it, but they now **throw `ConversionError`** on
///     unparseable input instead of silently returning 0 / 0.0. Binding
///     engines that fall back to this field route the exception through
///     `aria::report_callback_failure("binding.converter", …)` and skip
///     the Model write, so legacy callers gain the same safety as the
///     new channel without any source change.
///
/// User-defined converters MAY leave `try_to_model` empty; the engine
/// then uses `to_model` only.
template<typename T, typename U>
struct Converter {
    std::function<U(const T&)>                to_view;
    std::function<T(const U&)>                to_model;
    std::function<std::optional<T>(const U&)> try_to_model;
};

namespace converters {

inline Converter<std::string, std::string> identity_string() {
    return {
        [](const std::string& s) { return s; },
        [](const std::string& s) { return s; },
        [](const std::string& s) -> std::optional<std::string> { return s; }
    };
}

inline Converter<int, std::string> int_to_string() {
    return {
        [](const int& v) { return std::to_string(v); },
        [](const std::string& s) -> int {
            try {
                std::size_t consumed = 0;
                int v = std::stoi(s, &consumed);
                // Reject trailing garbage (e.g. "12abc"): stoi returns 12
                // but `consumed < s.size()` exposes the truncation.
                if (consumed != s.size()) {
                    throw ConversionError{"int_to_string: trailing chars"};
                }
                return v;
            } catch (const ConversionError&) {
                throw;
            } catch (...) {
                throw ConversionError{"int_to_string: not an integer"};
            }
        },
        [](const std::string& s) -> std::optional<int> {
            try {
                std::size_t consumed = 0;
                int v = std::stoi(s, &consumed);
                if (consumed != s.size()) return std::nullopt;
                return v;
            } catch (...) {
                return std::nullopt;
            }
        }
    };
}

inline Converter<double, std::string> double_to_string(int precision = 2) {
    auto to_view = [precision](const double& v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", precision, v);
        return std::string(buf);
    };
    auto to_model_strict = [](const std::string& s) -> double {
        try {
            std::size_t consumed = 0;
            double v = std::stod(s, &consumed);
            if (consumed != s.size()) {
                throw ConversionError{"double_to_string: trailing chars"};
            }
            return v;
        } catch (const ConversionError&) {
            throw;
        } catch (...) {
            throw ConversionError{"double_to_string: not a number"};
        }
    };
    auto try_to_model = [](const std::string& s) -> std::optional<double> {
        try {
            std::size_t consumed = 0;
            double v = std::stod(s, &consumed);
            if (consumed != s.size()) return std::nullopt;
            return v;
        } catch (...) {
            return std::nullopt;
        }
    };
    return { std::move(to_view), std::move(to_model_strict), std::move(try_to_model) };
}

inline Converter<bool, std::string> bool_to_yes_no() {
    return {
        [](const bool& v) -> std::string { return v ? "yes" : "no"; },
        [](const std::string& s) {
            return s == "yes" || s == "true" || s == "1";
        },
        [](const std::string& s) -> std::optional<bool> {
            if (s == "yes" || s == "true"  || s == "1") return true;
            if (s == "no"  || s == "false" || s == "0") return false;
            return std::nullopt;
        }
    };
}

}  // namespace converters
}  // namespace aria::binding
