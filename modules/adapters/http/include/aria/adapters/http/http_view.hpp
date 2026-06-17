#pragma once
/// @file http_view.hpp
/// @brief Logical "view" exposed over HTTP/REST/SSE.
///
/// Unlike Qt/AppKit/UIKit views which wrap a native widget pointer,
/// an HttpView is a *logical* view: it carries a stable string ID
/// (e.g. "search_keyword", "bookshelf_results"), a kind tag matching
/// the underlying control type ("text", "bool", "int", "click", ...),
/// and is registered in the HttpAdapter's view registry where the
/// adapter maintains the shadow state and routes incoming
/// REST messages to the right place.

#include "aria/abi/export.hpp"
#include "aria/binding/view_adapter.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace aria::adapters::http {

/// Logical view exposed over HTTP. See `wire_protocol.hpp` for the
/// full set of valid `kind` values.
///
/// Lifetime: the HttpView is owned by the HttpAdapter's view registry
/// (see HttpAdapter::register_view). Destroying the adapter (or calling
/// HttpAdapter::unregister_view) fires the IView::on_destroy signal so
/// any BindingEngine subscriptions wired to this view are released.
class ARIA_HTTP_API HttpView final : public binding::IView {
public:
    HttpView(std::string id, std::string kind)
        : id_(std::move(id)), kind_(std::move(kind)) {}

    [[nodiscard]] std::string_view kind() const noexcept override {
        return kind_;
    }

    /// Stable identifier used as routing key in HTTP/SSE messages.
    [[nodiscard]] const std::string& id() const noexcept { return id_; }

private:
    std::string id_;
    std::string kind_;
};

}  // namespace aria::adapters::http
