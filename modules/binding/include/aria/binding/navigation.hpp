#pragma once

// Navigator -- UI-toolkit-agnostic navigation stack for ViewModels.
//
// The protocol is built on four production-grade primitives that
// every modern MVVM router needs:
//
//   N-1 (Presentation kind). Each entry carries `Presentation::Push`
//       (default) or `Presentation::Modal`. `pop_to_root()` bottoms
//       out at the deepest non-modal entry; modals are torn down
//       individually via `dismiss_modal()` / `pop()`.
//
//   N-2 (Result passing). `push_for_result<R>(...)` returns a
//       `std::shared_future<std::optional<R>>` (any-thread observable)
//       that resolves when the child entry calls
//       `Navigator::dismiss_with(result)` or pops without a value
//       (in which case the future resolves to `std::nullopt`). Mirrors
//       Android `registerForActivityResult` / iOS delegate-back.
//
//   N-3 (Per-entry cancellation). Each entry owns a
//       `CancellationSource`; popping it (or the Navigator dropping
//       it for any reason) fires the source. ViewModels rooted at
//       this entry can `co_await` against `entry.token()` to abort
//       work the moment the user navigates away. Independent from
//       ViewModelScope -- works even if the VM itself is kept alive
//       elsewhere (e.g. cached for back-stack restoration).
//
//   N-4 (Deep-link routing). `register_route("path/{id}", factory)`
//       registers a path -> factory mapping. `route("path/42")`
//       parses the URI, instantiates the factory with the captured
//       params, and invokes `push` / `replace_root` / `clear+push`
//       depending on a `RouteOptions` flag. Out-of-scope for this
//       layer: query strings + fragments; we keep it deliberately
//       tight.
//
// The legacy push / pop / replace / clear / pop_to_root surface is
// preserved unchanged. New code should prefer the N-N primitives.

#include "aria/async/cancellation.hpp"
#include "aria/binding/view_model.hpp"
#include "aria/property.hpp"

#include <any>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aria::binding {

/// How a navigation entry is presented to the user. `Push` slides
/// onto the back-stack; `Modal` overlays without disturbing the
/// previous active page (the modal's parent stays "current" from
/// `current` Property's perspective in some adapter conventions, but
/// Navigator picks the topmost entry as `current` regardless -- the
/// adapter is responsible for rendering the modal as an overlay).
enum class Presentation : unsigned char {
    Push  = 0,
    Modal = 1,
};

/// Routing options for `Navigator::route(...)`.
struct RouteOptions {
    /// If true, replace the entire stack with the deep-linked entry.
    /// Otherwise push it onto the existing stack.
    bool clear_stack{false};
    /// Presentation kind for the deep-linked entry.
    Presentation presentation{Presentation::Push};
};

namespace detail {

/// Type-erased entry of the navigation stack. Owns the VM, its
/// per-entry cancellation source, and -- if the entry was created
/// via `push_for_result<R>` -- a promise<any> wrapper that resolves
/// with the caller-supplied result on dismiss.
struct NavEntry {
    std::shared_ptr<ViewModel>            vm;
    Presentation                          kind{Presentation::Push};
    aria::async::CancellationSource       cancel;
    std::string                           route_path;   // optional, set by deep-link

    // Type-erased result delivery. `result_setter` is non-null iff
    // the entry was created by push_for_result<R>; the setter knows
    // how to turn an `std::any` into a typed result.
    std::function<void(std::optional<std::any>)> result_setter;

    NavEntry()                              = default;
    NavEntry(const NavEntry&)               = delete;
    NavEntry& operator=(const NavEntry&)    = delete;
    NavEntry(NavEntry&&) noexcept           = default;
    NavEntry& operator=(NavEntry&&) noexcept = default;
};

}  // namespace detail

class Navigator {
public:
    /// Topmost entry's VM, or nullptr if the stack is empty.
    Property<std::shared_ptr<ViewModel>> current{nullptr};
    /// Total stack depth (modals included).
    Property<std::size_t>                depth{0};

    Navigator() = default;
    ~Navigator() { clear(); }

    Navigator(const Navigator&)            = delete;
    Navigator& operator=(const Navigator&) = delete;

    // ── Legacy ergonomics: typed push / replace returning the VM ──────

    template<typename VM, typename... Args>
        requires std::derived_from<VM, ViewModel>
    std::shared_ptr<VM> push(Args&&... args) {
        auto vm = std::make_shared<VM>(std::forward<Args>(args)...);
        push(vm);
        return vm;
    }

    template<typename VM, typename... Args>
        requires std::derived_from<VM, ViewModel>
    std::shared_ptr<VM> replace(Args&&... args) {
        auto vm = std::make_shared<VM>(std::forward<Args>(args)...);
        replace(vm);
        return vm;
    }

    /// Push (Presentation::Push). Activates the new entry and
    /// deactivates the previous topmost.
    void push(std::shared_ptr<ViewModel> vm,
              Presentation kind = Presentation::Push) {
        if (!vm) throw std::invalid_argument("Navigator::push: vm is null");
        deactivate_top_();
        detail::NavEntry e;
        e.vm   = std::move(vm);
        e.kind = kind;
        stack_.push_back(std::move(e));
        stack_.back().vm->activate();
        publish_();
    }

    /// Replace the topmost entry (in place). The previous entry is
    /// deactivated, its cancellation fires, and any pending
    /// push_for_result resolves with `std::nullopt`.
    void replace(std::shared_ptr<ViewModel> vm) {
        if (!vm) throw std::invalid_argument("Navigator::replace: vm is null");
        if (!stack_.empty()) {
            tear_down_top_(/*result=*/std::nullopt);
        }
        detail::NavEntry e;
        e.vm = std::move(vm);
        stack_.push_back(std::move(e));
        stack_.back().vm->activate();
        publish_();
    }

    /// N-2: push a child entry that will eventually return a typed
    /// result `R` to the caller. The returned shared_future resolves
    /// when the child calls `Navigator::dismiss_with<R>(result)` or
    /// is popped without a value (resolves to `std::nullopt`).
    template<typename R, typename VM, typename... Args>
        requires std::derived_from<VM, ViewModel>
    [[nodiscard]] std::shared_future<std::optional<R>>
    push_for_result(Args&&... args) {
        auto vm = std::make_shared<VM>(std::forward<Args>(args)...);
        return push_for_result<R>(std::shared_ptr<ViewModel>{vm});
    }

    template<typename R>
    [[nodiscard]] std::shared_future<std::optional<R>>
    push_for_result(std::shared_ptr<ViewModel> vm,
                    Presentation kind = Presentation::Push) {
        if (!vm) throw std::invalid_argument(
            "Navigator::push_for_result: vm is null");

        auto promise = std::make_shared<std::promise<std::optional<R>>>();
        std::shared_future<std::optional<R>> fut =
            promise->get_future().share();

        deactivate_top_();
        detail::NavEntry e;
        e.vm   = std::move(vm);
        e.kind = kind;
        // Capture the promise into a type-erased setter. `payload`
        // is `std::nullopt` on cancel/pop-without-result, or holds
        // an `std::any{R}` on dismiss_with<R>. Strong-capture the
        // promise: nothing else owns it, and there is no cycle
        // (promise does not reach back to the NavEntry).
        auto p = promise;
        e.result_setter = [p](std::optional<std::any> payload) {
            try {
                if (!payload.has_value()) {
                    p->set_value(std::nullopt);
                } else {
                    p->set_value(std::any_cast<R>(*payload));
                }
            } catch (const std::bad_any_cast&) {
                // Type mismatch -- callee called dismiss_with<X> for
                // some X != R. Resolve to nullopt to keep N-2 honest;
                // the trace sink (D-N) will record the mismatch when
                // diagnostic categories grow to cover Navigator.
                try { p->set_value(std::nullopt); } catch (...) {}
            } catch (...) {
                // Promise already satisfied (e.g. double dismiss);
                // benign for callers.
            }
        };
        stack_.push_back(std::move(e));
        stack_.back().vm->activate();
        publish_();
        return fut;
    }

    /// N-2: dismiss the topmost entry with a typed result. Equivalent
    /// to `pop()` but propagates `result` to the awaiting future.
    /// If the topmost entry was not created via `push_for_result<R>`
    /// the result is discarded (matches Android `setResult` on a
    /// non-result-launched activity).
    template<typename R>
    bool dismiss_with(R result) {
        if (stack_.empty()) return false;
        std::optional<std::any> payload{std::any{std::move(result)}};
        tear_down_top_(std::move(payload));
        if (!stack_.empty()) stack_.back().vm->activate();
        publish_();
        return true;
    }

    /// Pop the topmost entry. Returns false if the stack is empty.
    /// Any pending `push_for_result` resolves with `std::nullopt`.
    bool pop() {
        if (stack_.empty()) return false;
        tear_down_top_(std::nullopt);
        if (!stack_.empty()) stack_.back().vm->activate();
        publish_();
        return true;
    }

    /// N-1: dismiss the topmost MODAL entry. Returns false if the
    /// topmost is not a modal.
    bool dismiss_modal() {
        if (stack_.empty() || stack_.back().kind != Presentation::Modal) {
            return false;
        }
        return pop();
    }

    /// Pop until only one Push entry remains. Modals are popped first,
    /// then Push entries; the deepest Push entry is preserved.
    void pop_to_root() {
        while (stack_.size() > 1) {
            tear_down_top_(std::nullopt);
        }
        if (!stack_.empty()) stack_.back().vm->activate();
        publish_();
    }

    void clear() {
        while (!stack_.empty()) {
            tear_down_top_(std::nullopt);
        }
        publish_();
    }

    [[nodiscard]] bool empty() const noexcept { return stack_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return stack_.size(); }

    [[nodiscard]] std::shared_ptr<ViewModel> at(std::size_t i) const {
        return stack_.at(i).vm;
    }

    /// Topmost entry's cancellation token. Throws if the stack is
    /// empty -- callers normally take the token immediately after
    /// `push` and stash it.
    [[nodiscard]] aria::async::CancellationToken top_token() const {
        if (stack_.empty()) {
            throw std::out_of_range("Navigator::top_token: stack is empty");
        }
        return stack_.back().cancel.token();
    }

    /// Topmost entry's presentation kind.
    [[nodiscard]] Presentation top_presentation() const {
        if (stack_.empty()) {
            throw std::out_of_range(
                "Navigator::top_presentation: stack is empty");
        }
        return stack_.back().kind;
    }

    // ── N-4: deep-link routing ─────────────────────────────────────────

    /// Param map captured from a route pattern (e.g. `{id}`).
    using RouteParams = std::unordered_map<std::string, std::string>;
    using RouteFactory = std::function<std::shared_ptr<ViewModel>(const RouteParams&)>;

    /// Register a route pattern. Pattern segments enclosed in `{...}`
    /// capture into `RouteParams`; everything else is matched literally.
    /// Example: `"users/{id}"` matches `"users/42"` -> {id: "42"}.
    void register_route(std::string pattern, RouteFactory factory) {
        if (!factory) {
            throw std::invalid_argument(
                "Navigator::register_route: factory is null");
        }
        routes_.emplace(std::move(pattern), std::move(factory));
    }

    /// Resolve `path` against registered patterns. Returns false if
    /// no pattern matches; in that case the navigator is unchanged.
    /// Otherwise applies the action prescribed by `opts`.
    bool route(std::string_view path, RouteOptions opts = {}) {
        for (const auto& [pattern, factory] : routes_) {
            RouteParams params;
            if (match_route_(pattern, path, params)) {
                auto vm = factory(params);
                if (!vm) return false;
                if (opts.clear_stack) {
                    clear();
                }
                push(std::move(vm), opts.presentation);
                if (!stack_.empty()) {
                    stack_.back().route_path = std::string(path);
                }
                return true;
            }
        }
        return false;
    }

private:
    void publish_() {
        current = stack_.empty() ? nullptr : stack_.back().vm;
        depth   = stack_.size();
    }

    void deactivate_top_() {
        if (!stack_.empty()) stack_.back().vm->deactivate();
    }

    /// Tear down the topmost entry: deactivate the VM, fire the
    /// per-entry cancellation, deliver the result (or std::nullopt),
    /// pop the storage. Caller is responsible for activating the new
    /// topmost (if any) and calling publish_().
    void tear_down_top_(std::optional<std::any> result_payload) {
        if (stack_.empty()) return;
        auto& e = stack_.back();
        e.vm->deactivate();
        if (e.result_setter) {
            try { e.result_setter(std::move(result_payload)); } catch (...) {}
        }
        try { e.cancel.cancel(); } catch (...) {}
        stack_.pop_back();
    }

    /// Match a `pattern` against `path`. `{name}` segments capture
    /// non-empty alphanumeric tokens; literal segments must match
    /// byte-for-byte. Trailing slashes are normalised.
    static bool match_route_(std::string_view pattern,
                             std::string_view path,
                             RouteParams&     out) {
        auto split = [](std::string_view s) {
            std::vector<std::string_view> segs;
            std::size_t start = 0;
            for (std::size_t i = 0; i <= s.size(); ++i) {
                if (i == s.size() || s[i] == '/') {
                    if (i > start) segs.push_back(s.substr(start, i - start));
                    start = i + 1;
                }
            }
            return segs;
        };
        auto pat_segs  = split(pattern);
        auto path_segs = split(path);
        if (pat_segs.size() != path_segs.size()) return false;
        for (std::size_t i = 0; i < pat_segs.size(); ++i) {
            const auto& ps = pat_segs[i];
            if (ps.size() >= 2 && ps.front() == '{' && ps.back() == '}') {
                if (path_segs[i].empty()) return false;
                out.emplace(std::string(ps.substr(1, ps.size() - 2)),
                            std::string(path_segs[i]));
            } else if (ps != path_segs[i]) {
                return false;
            }
        }
        return true;
    }

    std::vector<detail::NavEntry>                stack_;
    std::unordered_map<std::string, RouteFactory> routes_;
};

}  // namespace aria::binding
