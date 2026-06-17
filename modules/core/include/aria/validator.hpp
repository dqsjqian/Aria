#pragma once

#include "aria/concepts.hpp"
#include "aria/diagnostics.hpp"
#include "aria/error.hpp"
#include "aria/property.hpp"
#include "aria/subscription.hpp"
#include "aria/validation_key.hpp"

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aria {

// ---------------------------------------------------------------------------
//  ValidationResult -- legacy "valid + errors" projection
//
//  Kept for callers that just want a quick boolean + list. New code should
//  bind to `Validator::state()` directly: it carries the same data plus
//  `touched / dirty / pending`.
//
//  Per docs/error-model.md the entries are unified `aria::Error`s with
//  `kind == ErrorKind::Validation` and a populated `key`.
// ---------------------------------------------------------------------------
struct ValidationResult {
    bool valid = true;
    std::vector<Error> errors;

    explicit operator bool() const noexcept { return valid; }

    /// Project the rich error list down to plain message strings, in
    /// the order they were produced. Useful for legacy bindings that
    /// only know how to display strings.
    [[nodiscard]] std::vector<std::string> error_messages() const {
        std::vector<std::string> out;
        out.reserve(errors.size());
        for (const auto& e : errors) out.push_back(e.message);
        return out;
    }
};

inline bool operator==(const ValidationResult& a, const ValidationResult& b) noexcept {
    return a.valid == b.valid && a.errors == b.errors;
}
inline bool operator!=(const ValidationResult& a, const ValidationResult& b) noexcept {
    return !(a == b);
}

// ---------------------------------------------------------------------------
//  ValidationState -- the richer form-state record
// ---------------------------------------------------------------------------
struct ValidationState {
    /// True iff there are no Error-severity entries. Warnings do NOT
    /// make a field invalid.
    bool valid = true;

    /// True while an async validator is running.
    bool pending = false;

    /// True once the user has interacted with the field (typically on
    /// focus-out).
    bool touched = false;

    /// True once the field's value has moved away from its baseline.
    bool dirty = false;

    /// Hard failures (kind == Validation, severity == Error). These
    /// drive `valid = false`.
    std::vector<Error> errors;

    /// Soft advisories (kind == Validation, severity == Warning). DO
    /// NOT flip `valid`.
    std::vector<Error> warnings;

    // ── Convenience queries (dominant UI patterns) ────────────────────

    /// First Error-severity entry, if any.
    [[nodiscard]] std::optional<Error> first_error() const {
        if (errors.empty()) return std::nullopt;
        return errors.front();
    }

    /// First Error-severity message text, if any.
    [[nodiscard]] std::optional<std::string> first_error_message() const {
        if (errors.empty()) return std::nullopt;
        return errors.front().message;
    }

    /// All Error-severity entries that target a given field path.
    [[nodiscard]] std::vector<Error>
    errors_for(std::string_view field_path) const {
        std::vector<Error> out;
        for (const auto& e : errors) {
            if (e.key.field_path == field_path) out.push_back(e);
        }
        return out;
    }

    /// True if any Error-severity entry has the given `rule_id`.
    [[nodiscard]] bool has_error_with_rule(std::string_view rule_id) const noexcept {
        for (const auto& e : errors) {
            if (e.key.rule_id == rule_id) return true;
        }
        return false;
    }

    explicit operator bool() const noexcept { return valid; }
};

inline bool operator==(const ValidationState& a, const ValidationState& b) noexcept {
    return a.valid    == b.valid
        && a.pending  == b.pending
        && a.touched  == b.touched
        && a.dirty    == b.dirty
        && a.errors   == b.errors
        && a.warnings == b.warnings;
}
inline bool operator!=(const ValidationState& a, const ValidationState& b) noexcept {
    return !(a == b);
}

// ---------------------------------------------------------------------------
//  Validator<T>
// ---------------------------------------------------------------------------
template<PropertyValue T>
class Validator {
public:
    /// User-supplied rule body: returns a message when the value
    /// fails, `std::nullopt` when it passes. The framework wraps the
    /// returned message into an `Error` (kind = Validation) together
    /// with the owning Validator's `field_path` and either an
    /// explicit or auto-generated `rule_id`.
    using Rule = std::function<std::optional<std::string>(const T&)>;

    explicit Validator(Property<T>& source, std::string field_path = {})
        : source_(&source),
          field_path_(std::move(field_path)),
          baseline_(source.get()),
          result_(ValidationResult{true, {}}),
          state_(ValidationState{}) {
        sub_ = source_->bind([this](const T& v) {
            const bool new_dirty = !(v == baseline_);
            if (new_dirty != state_.peek().dirty) {
                auto s = state_.peek();
                s.dirty = new_dirty;
                state_.set(s);
            }
            run_(v);
        });
    }

    [[nodiscard]] const std::string& field_path() const noexcept {
        return field_path_;
    }

    // ── Rule composition ──────────────────────────────────────────────

    Validator& rule(Rule r, std::string rule_id_value = {}) {
        const std::size_t auto_id = auto_id_counter_++;
        if (rule_id_value.empty()) {
            rule_id_value = "rule_" + std::to_string(auto_id);
        }
        rules_.push_back(RuleEntry{std::move(r), std::move(rule_id_value)});
        if (!suspend_) run_(source_->get());
        return *this;
    }

    template<std::predicate<const T&> P>
    Validator& must(P&& predicate, std::string message,
                    std::string rule_id_value = {}) {
        return rule(
            [p = std::forward<P>(predicate), m = std::move(message)](
                const T& v) -> std::optional<std::string> {
                if (p(v)) return std::nullopt;
                return m;
            },
            std::move(rule_id_value));
    }

    Validator& warning(Rule r, std::string rule_id_value = {}) {
        const std::size_t auto_id = auto_id_counter_++;
        if (rule_id_value.empty()) {
            rule_id_value = "rule_" + std::to_string(auto_id);
        }
        warnings_.push_back(RuleEntry{std::move(r), std::move(rule_id_value)});
        if (!suspend_) run_(source_->get());
        return *this;
    }

    template<std::predicate<const T&> P>
    Validator& should(P&& predicate, std::string message,
                      std::string rule_id_value = {}) {
        return warning(
            [p = std::forward<P>(predicate), m = std::move(message)](
                const T& v) -> std::optional<std::string> {
                if (p(v)) return std::nullopt;
                return m;
            },
            std::move(rule_id_value));
    }

    Validator& rules(std::initializer_list<Rule> rs) {
        suspend_ = true;
        for (const auto& r : rs) {
            const std::size_t auto_id = auto_id_counter_++;
            rules_.push_back(RuleEntry{r, "rule_" + std::to_string(auto_id)});
        }
        suspend_ = false;
        run_(source_->get());
        return *this;
    }

    // ── Form-state transitions ────────────────────────────────────────

    void touch() {
        if (state_.peek().touched) return;
        auto s = state_.peek();
        s.touched = true;
        state_.set(s);
    }

    void reset_touched() {
        if (!state_.peek().touched) return;
        auto s = state_.peek();
        s.touched = false;
        state_.set(s);
    }

    void reset_dirty() {
        // `peek_ref()` instead of `get()`/`get_ref()` so that calling
        // reset_dirty() from inside a Computed/Effect doesn't make that
        // Derivation accidentally depend on the source — `reset_dirty`
        // is administrative and should never establish a reactive edge.
        baseline_ = source_->peek_ref();
        if (!state_.peek_ref().dirty) return;
        auto s = state_.peek_ref();
        s.dirty = false;
        state_.set(std::move(s));
    }

    void begin_pending() {
        if (state_.peek().pending) return;
        auto s = state_.peek();
        s.pending = true;
        state_.set(s);
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Validation,
                ::aria::trace::Validation{
                    "begin_pending",
                    ::aria::ValidationKey{field_path_, std::string{}},
                    std::string{},
                });
        }
    }

    /// Settle the async validation: clears `pending` and stores
    /// `extra_messages` as additional Error-severity entries until
    /// the next sync run replaces them. Each entry is keyed under
    /// this validator's `field_path` and an auto-generated
    /// `async_<N>` rule_id.
    void end_pending(std::vector<std::string> extra_messages) {
        async_errors_.clear();
        async_errors_.reserve(extra_messages.size());
        std::size_t i = 0;
        for (auto& msg : extra_messages) {
            async_errors_.push_back(Error::validation(
                ValidationKey{field_path_, "async_" + std::to_string(i++)},
                std::move(msg)));
        }
        finish_pending_();
    }

    /// Settle pending with caller-shaped error records. Empty
    /// `field_path`s are backfilled with the validator's own.
    void end_pending(std::vector<Error> extra) {
        async_errors_ = std::move(extra);
        finish_pending_();
    }

    /// Settle pending without any extra errors.
    void end_pending() {
        async_errors_.clear();
        finish_pending_();
    }

    // ── Accessors ─────────────────────────────────────────────────────

    [[nodiscard]] Property<ValidationState>& state() noexcept { return state_; }
    [[nodiscard]] const Property<ValidationState>& state() const noexcept { return state_; }

    [[nodiscard]] Property<ValidationResult>& result() noexcept { return result_; }
    [[nodiscard]] const Property<ValidationResult>& result() const noexcept { return result_; }

private:
    struct RuleEntry {
        Rule        body;
        std::string rule_id;
    };

    void finish_pending_() {
        {
            auto s = state_.peek();
            s.pending = false;
            state_.set(s);
        }
        if (::aria::has_trace_sink()) {
            ::aria::publish_trace_unchecked(::aria::TraceCategory::Validation,
                ::aria::trace::Validation{
                    "end_pending",
                    ::aria::ValidationKey{field_path_, std::string{}},
                    std::string{},
                });
        }
        run_(source_->get());
    }

    void run_(const T& v) {
        const bool tracing = ::aria::has_trace_sink();
        auto s = state_.peek_ref();

        s.errors.clear();
        for (auto& entry : rules_) {
            if (auto msg = entry.body(v)) {
                if (tracing) {
                    ::aria::publish_trace_unchecked(::aria::TraceCategory::Validation,
                        ::aria::trace::Validation{
                            "rule_fail",
                            ::aria::ValidationKey{field_path_, entry.rule_id},
                            *msg,
                        });
                }
                s.errors.push_back(Error::validation(
                    ValidationKey{field_path_, entry.rule_id},
                    std::move(*msg)));
            } else if (tracing) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Validation,
                    ::aria::trace::Validation{
                        "rule_pass",
                        ::aria::ValidationKey{field_path_, entry.rule_id},
                        std::string{},
                    });
            }
        }
        for (const auto& e : async_errors_) {
            // Backfill field_path / kind / source defaults so the
            // resulting Error is well-formed regardless of how the
            // caller constructed it.
            Error fixed = e;
            if (fixed.kind != ErrorKind::Validation) {
                fixed.kind = ErrorKind::Validation;
            }
            if (fixed.key.field_path.empty()) {
                fixed.key.field_path = field_path_;
            }
            if (fixed.source.empty()) {
                fixed.source = "Validator";
            }
            s.errors.push_back(std::move(fixed));
        }

        s.warnings.clear();
        for (auto& entry : warnings_) {
            if (auto msg = entry.body(v)) {
                if (tracing) {
                    ::aria::publish_trace_unchecked(::aria::TraceCategory::Validation,
                        ::aria::trace::Validation{
                            "warning_fail",
                            ::aria::ValidationKey{field_path_, entry.rule_id},
                            *msg,
                        });
                }
                s.warnings.push_back(Error::validation_warning(
                    ValidationKey{field_path_, entry.rule_id},
                    std::move(*msg)));
            } else if (tracing) {
                ::aria::publish_trace_unchecked(::aria::TraceCategory::Validation,
                    ::aria::trace::Validation{
                        "warning_pass",
                        ::aria::ValidationKey{field_path_, entry.rule_id},
                        std::string{},
                    });
            }
        }

        s.valid = s.errors.empty();

        // Coalesce the two writes into a single graph flush. Without
        // the batch, observers wired to BOTH `state()` and `result()`
        // would re-render twice for one validation pass — and
        // intermediate observers might briefly see `state.valid=true`
        // while `result.valid=false` (or vice versa). The batch makes
        // the two updates atomic from the observer's point of view.
        ValidationResult new_result{s.valid, s.errors};
        ::aria::reactive::batch([&]{
            state_.set(std::move(s));
            result_.set(std::move(new_result));
        });
    }

    Property<T>*                source_;
    std::string                 field_path_;
    T                           baseline_;
    std::vector<RuleEntry>      rules_;
    std::vector<RuleEntry>      warnings_;
    std::vector<Error>          async_errors_;
    Property<ValidationResult>  result_;
    Property<ValidationState>   state_;
    Subscription                sub_;
    std::size_t                 auto_id_counter_ = 0;
    bool                        suspend_ = false;
};

}  // namespace aria
