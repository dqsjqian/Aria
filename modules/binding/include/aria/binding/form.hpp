#pragma once

// FormField / FormGroup / FormValidator -- light client-side form helpers.
//
// FormField bundles the common trio:
//   Property<T> value
//   Validator<T> validator
//   Property<bool> is_valid
//   Property<std::string> error
//   Property<std::optional<aria::Error>> error_full
//
// Every FormField carries a `field_path` (e.g. "user.email") that flows
// into its underlying Validator, so each emitted Error is routed back
// to the cell with full provenance. See `docs/error-model.md` for the
// rationale.

#include "aria/error.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"
#include "aria/validation_key.hpp"
#include "aria/validator.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aria::binding {

template<PropertyValue T>
class FormField {
public:
    Property<T> value;
    Validator<T> validator;
    Property<bool> is_valid{true};
    /// First-error message string for trivial UI binding. For full
    /// provenance use `error_full` or `validator.state()`.
    Property<std::string> error{""};
    /// Full first-error record. Carries the same message as `error`
    /// when present, plus `kind` / `key` / `severity`.
    Property<std::optional<::aria::Error>> error_full{std::nullopt};
    Property<bool> touched{false};
    Property<bool> dirty{false};

    /// Construct from `(field_path, initial)`. The path flows into
    /// the owned Validator so every emitted Error is keyed by it.
    explicit FormField(std::string field_path, T initial = T{})
        : value(std::move(initial)),
          validator(value, std::move(field_path)),
          initial_(value.get()) {
        wire_();
    }

    [[nodiscard]] const std::string& field_path() const noexcept {
        return validator.field_path();
    }

    FormField& rule(typename Validator<T>::Rule r,
                    std::string rule_id_value = {}) {
        validator.rule(std::move(r), std::move(rule_id_value));
        return *this;
    }

    template<std::predicate<const T&> P>
    FormField& must(P&& predicate, std::string message,
                    std::string rule_id_value = {}) {
        validator.must(std::forward<P>(predicate),
                       std::move(message),
                       std::move(rule_id_value));
        return *this;
    }

    FormField& required(std::string message = "required")
        requires requires(const T& v) { { v.empty() } -> std::convertible_to<bool>; }
    {
        return must([](const T& v) { return !v.empty(); },
                    std::move(message),
                    "required");
    }

    FormField& min_length(std::size_t n, std::string message)
        requires requires(const T& v) { { v.size() } -> std::convertible_to<std::size_t>; }
    {
        return must([n](const T& v) { return v.size() >= n; },
                    std::move(message),
                    "min_length");
    }

    void reset(T v = T{}) {
        initial_ = v;
        value = std::move(v);
        touched = false;
        dirty = false;
    }

private:
    void wire_() {
        bag_ += validator.result().bind([this](const ValidationResult& r) {
            is_valid = r.valid;
            if (r.valid || r.errors.empty()) {
                error = "";
                error_full = std::nullopt;
            } else {
                error = r.errors.front().message;
                error_full = r.errors.front();
            }
        });
        bag_ += value.on_changed([this](const T& v) {
            touched = true;
            dirty = !(v == initial_);
        });
    }

    T initial_;
    SubscriptionBag bag_;
};

class FormGroup {
public:
    Property<bool> is_valid{true};
    Property<bool> is_dirty{false};

    template<typename Field>
    void track(Field& f) {
        fields_.push_back(FieldHooks{
            [&f] { return f.is_valid.get(); },
            [&f] { return f.dirty.get(); }
        });
        bag_ += f.is_valid.on_changed([this](bool) { recompute_(); });
        bag_ += f.dirty.on_changed([this](bool) { recompute_(); });
        recompute_();
    }

    void clear() {
        fields_.clear();
        bag_.clear();
        is_valid = true;
        is_dirty = false;
    }

private:
    struct FieldHooks {
        std::function<bool()> valid;
        std::function<bool()> dirty;
    };

    void recompute_() {
        bool all_valid = true;
        bool any_dirty = false;
        for (auto& f : fields_) {
            all_valid = all_valid && f.valid();
            any_dirty = any_dirty || f.dirty();
        }
        is_valid = all_valid;
        is_dirty = any_dirty;
    }

    std::vector<FieldHooks> fields_;
    SubscriptionBag bag_;
};

// ============================================================================
//  FormValidator -- form-level validation aggregator.
//
//  Cross-field rules are keyed by `ValidationKey { "", rule_id }` so a
//  UI consumer can still route the message even though the rule is
//  not anchored to any single field. Per-field errors retain their
//  original key from the underlying Validator.
// ============================================================================
class FormValidator {
public:
    Property<bool>                                  is_valid{true};
    Property<bool>                                  is_dirty{false};
    Property<bool>                                  is_pending{false};
    /// First-error message (string). Empty when the form is valid.
    Property<std::string>                           first_error{""};
    /// First-error record with kind / key / severity.
    Property<std::optional<::aria::Error>>          first_error_full{std::nullopt};

    template<typename Field>
    void track(Field& f) {
        track_(FieldHooks{
            [&f] { return f.is_valid.get(); },
            [&f] { return f.dirty.get(); },
            [] { return false; /* sync FormField has no pending */ },
            [&f] { return f.error_full.get(); },
        });
        bag_ += f.is_valid.on_changed   ([this](bool) { recompute_(); });
        bag_ += f.dirty.on_changed      ([this](bool) { recompute_(); });
        bag_ += f.error_full.on_changed ([this](const std::optional<::aria::Error>&) {
            recompute_();
        });
        using ValueT = std::remove_reference_t<decltype(f.value.get())>;
        bag_ += f.value.on_changed([this](const ValueT&) { recompute_(); });
    }

    /// Add a cross-field rule. `rule_id` defaults to
    /// `"form_rule_<N>"` when omitted; emitted Error has empty
    /// `field_path` (rules are not anchored to a single field) and
    /// `source = "FormValidator"`.
    template<std::predicate Pred>
    void rule(Pred predicate, std::string message,
              std::string rule_id_value = {}) {
        if (rule_id_value.empty()) {
            rule_id_value = "form_rule_" + std::to_string(rules_.size());
        }
        rules_.push_back(Rule{
            std::function<bool()>(std::move(predicate)),
            std::move(message),
            std::move(rule_id_value),
        });
        recompute_();
    }

    void clear() {
        fields_.clear();
        rules_.clear();
        bag_.clear();
        is_valid          = true;
        is_dirty          = false;
        is_pending        = false;
        first_error       = "";
        first_error_full  = std::nullopt;
    }

private:
    struct FieldHooks {
        std::function<bool()>                          valid;
        std::function<bool()>                          dirty;
        std::function<bool()>                          pending;
        std::function<std::optional<::aria::Error>()>  error;
    };
    struct Rule {
        std::function<bool()> predicate;
        std::string           message;
        std::string           rule_id;
    };

    void track_(FieldHooks h) {
        fields_.push_back(std::move(h));
        recompute_();
    }

    void recompute_() {
        bool all_valid   = true;
        bool any_dirty   = false;
        bool any_pending = false;
        std::optional<::aria::Error> headline;

        for (auto& r : rules_) {
            if (!r.predicate()) {
                all_valid = false;
                if (!headline) {
                    auto e = ::aria::Error::validation(
                        ValidationKey{std::string{}, r.rule_id},
                        r.message);
                    e.source = "FormValidator";
                    headline = std::move(e);
                }
            }
        }
        for (auto& f : fields_) {
            const bool v = f.valid();
            const bool d = f.dirty();
            const bool p = f.pending();
            all_valid   = all_valid   && v;
            any_dirty   = any_dirty   || d;
            any_pending = any_pending || p;
            if (!headline && !v) {
                headline = f.error();
            }
        }

        is_valid          = all_valid;
        is_dirty          = any_dirty;
        is_pending        = any_pending;
        first_error_full  = headline;
        first_error       = headline ? headline->message : std::string{};
    }

    std::vector<FieldHooks> fields_;
    std::vector<Rule>       rules_;
    SubscriptionBag         bag_;
};

}  // namespace aria::binding
