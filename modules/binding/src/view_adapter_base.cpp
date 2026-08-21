// ============================================================================
//  view_adapter_base.cpp — ViewAdapterBase's compliant "unsupported" defaults.
//
//  Every operation funnels through `report_unsupported` and then returns a
//  safe value, so an adapter that inherits a channel it does not implement
//  degrades exactly the way contract L-39 requires: visible in diagnostics,
//  harmless at runtime.
// ============================================================================

#include "aria/binding/view_adapter_base.hpp"

#include "aria/runtime/logger.hpp"

#include <string>

namespace aria::binding {

ViewAdapterBase::~ViewAdapterBase() = default;

void ViewAdapterBase::report_unsupported(std::string_view op,
                                         const IView& v) const {
    // Match L-39's first-party adapter shape: stable per-platform category,
    // operation name, and the unsupported view kind.
    std::string category{platform_name()};
    category.append("_adapter");
    std::string msg;
    msg.reserve(op.size() + v.kind().size() + 40);
    msg.append(op)
       .append(": no binding path for view kind '")
       .append(v.kind())
       .append("'");
    aria::runtime::Logger::instance().warn(category, msg);
}

// ── Text ───────────────────────────────────────────────────────────────────
void ViewAdapterBase::set_text(IView& v, std::string_view) {
    report_unsupported("set_text", v);
}
std::string ViewAdapterBase::get_text(IView& v) {
    report_unsupported("get_text", v);
    return {};
}
Subscription ViewAdapterBase::on_text_changed(
    IView& v, std::function<void(std::string_view)>) {
    report_unsupported("on_text_changed", v);
    return {};
}

// ── Bool ───────────────────────────────────────────────────────────────────
void ViewAdapterBase::set_bool(IView& v, bool) {
    report_unsupported("set_bool", v);
}
bool ViewAdapterBase::get_bool(IView& v) {
    report_unsupported("get_bool", v);
    return false;
}
Subscription ViewAdapterBase::on_bool_changed(
    IView& v, std::function<void(bool)>) {
    report_unsupported("on_bool_changed", v);
    return {};
}

// ── Int ────────────────────────────────────────────────────────────────────
void ViewAdapterBase::set_int(IView& v, int) {
    report_unsupported("set_int", v);
}
int ViewAdapterBase::get_int(IView& v) {
    report_unsupported("get_int", v);
    return 0;
}
Subscription ViewAdapterBase::on_int_changed(
    IView& v, std::function<void(int)>) {
    report_unsupported("on_int_changed", v);
    return {};
}

// ── Int64 ──────────────────────────────────────────────────────────────────
void ViewAdapterBase::set_int64(IView& v, std::int64_t) {
    report_unsupported("set_int64", v);
}
std::int64_t ViewAdapterBase::get_int64(IView& v) {
    report_unsupported("get_int64", v);
    return 0;
}
Subscription ViewAdapterBase::on_int64_changed(
    IView& v, std::function<void(std::int64_t)>) {
    report_unsupported("on_int64_changed", v);
    return {};
}

// ── UInt64 ─────────────────────────────────────────────────────────────────
void ViewAdapterBase::set_uint64(IView& v, std::uint64_t) {
    report_unsupported("set_uint64", v);
}
std::uint64_t ViewAdapterBase::get_uint64(IView& v) {
    report_unsupported("get_uint64", v);
    return 0;
}
Subscription ViewAdapterBase::on_uint64_changed(
    IView& v, std::function<void(std::uint64_t)>) {
    report_unsupported("on_uint64_changed", v);
    return {};
}

// ── Float ──────────────────────────────────────────────────────────────────
void ViewAdapterBase::set_float(IView& v, float) {
    report_unsupported("set_float", v);
}
float ViewAdapterBase::get_float(IView& v) {
    report_unsupported("get_float", v);
    return 0.0f;
}
Subscription ViewAdapterBase::on_float_changed(
    IView& v, std::function<void(float)>) {
    report_unsupported("on_float_changed", v);
    return {};
}

// ── Double ─────────────────────────────────────────────────────────────────
void ViewAdapterBase::set_double(IView& v, double) {
    report_unsupported("set_double", v);
}
double ViewAdapterBase::get_double(IView& v) {
    report_unsupported("get_double", v);
    return 0.0;
}
Subscription ViewAdapterBase::on_double_changed(
    IView& v, std::function<void(double)>) {
    report_unsupported("on_double_changed", v);
    return {};
}

// ── Visible / Enabled ──────────────────────────────────────────────────────
void ViewAdapterBase::set_visible(IView& v, bool) {
    report_unsupported("set_visible", v);
}
void ViewAdapterBase::set_enabled(IView& v, bool) {
    report_unsupported("set_enabled", v);
}

// ── Click ──────────────────────────────────────────────────────────────────
Subscription ViewAdapterBase::on_click(IView& v, std::function<void()>) {
    report_unsupported("on_click", v);
    return {};
}

}  // namespace aria::binding
