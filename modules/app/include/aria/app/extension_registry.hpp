#pragma once

#include <aria/detail/typed_signal.hpp>
#include <aria/subscription.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria::app {

/// Single-provider extension points. Use on the application's owner thread.
/// Values are copied before callbacks, so a factory may remove or replace its
/// registration. Keep the returned Subscription for the contribution lifetime.
/// A stale handle never removes a later registration under the same key.
template<class Key, class Value, class Hash = std::hash<Key>>
class ExtensionRegistry {
    struct Record {
        Value value;
        std::uint64_t generation;
        bool enabled{true};
    };
    struct State {
        std::unordered_map<Key, Record, Hash> entries;
        std::uint64_t next_generation{0};
        aria::detail::TypedSignal<> changed;
    };
    std::shared_ptr<State> state_{std::make_shared<State>()};
public:
    ExtensionRegistry() = default;
    ExtensionRegistry(const ExtensionRegistry&) = delete;
    ExtensionRegistry& operator=(const ExtensionRegistry&) = delete;

    [[nodiscard]] aria::Subscription add(Key key, Value value) {
        auto state = state_;
        if (state->entries.contains(key))
            throw std::invalid_argument("duplicate extension registration");
        const auto generation = ++state->next_generation;
        aria::Subscription token{[weak = std::weak_ptr<State>(state), key, generation] {
            if (auto live = weak.lock()) {
                auto found = live->entries.find(key);
                if (found != live->entries.end() && found->second.generation == generation) {
                    auto removed = live->entries.extract(found);
                    live->changed.emit();
                }
            }
        }};
        state->entries.emplace(std::move(key), Record{std::move(value), generation});
        state->changed.emit();
        return token;
    }

    [[nodiscard]] std::optional<Value> find(const Key& key) const {
        auto found = state_->entries.find(key);
        if (found == state_->entries.end() || !found->second.enabled) return std::nullopt;
        return found->second.value;
    }
    [[nodiscard]] bool contains(const Key& key) const { return generation(key) != 0; }
    /// Identity of the enabled registration; zero means absent or disabled.
    [[nodiscard]] std::uint64_t generation(const Key& key) const {
        auto found = state_->entries.find(key);
        return found == state_->entries.end() || !found->second.enabled
            ? 0 : found->second.generation;
    }
    bool remove(const Key& key) {
        auto state = state_;
        auto removed = state->entries.extract(key);
        if (removed.empty()) return false;
        state->changed.emit();
        return true;
    }
    bool set_enabled(const Key& key, bool enabled) {
        auto state = state_;
        auto found = state->entries.find(key);
        if (found == state->entries.end()) return false;
        if (found->second.enabled != enabled) {
            found->second.enabled = enabled;
            state->changed.emit();
        }
        return true;
    }
    void clear() {
        auto state = state_;
        if (state->entries.empty()) return;
        decltype(state->entries) removed;
        removed.swap(state->entries);
        state->changed.emit();
    }
    [[nodiscard]] aria::Subscription on_changed(std::function<void()> callback) {
        return state_->changed.connect(std::move(callback));
    }
    /// Enabled contributions in registration order, independent of hash order.
    [[nodiscard]] std::vector<std::pair<Key, Value>> snapshot() const {
        std::vector<std::pair<std::uint64_t, std::pair<Key, Value>>> ordered;
        for (const auto& [key, entry] : state_->entries) {
            if (entry.enabled) ordered.push_back({entry.generation, {key, entry.value}});
        }
        std::ranges::sort(ordered, {}, &decltype(ordered)::value_type::first);
        std::vector<std::pair<Key, Value>> result;
        result.reserve(ordered.size());
        for (auto& entry : ordered) result.push_back(std::move(entry.second));
        return result;
    }
};

} // namespace aria::app
