// AppKitAdapter.mm — production-grade IViewAdapter for macOS AppKit.
//
// Mirrors the Qt6 adapter's bridge pattern (per-view, per-kind cache of
// `abi::SignalErased` + native ObjC target/delegate). Subscriptions
// returned from on_*_changed / on_click detach properly via
// `SignalErased::disconnect_via_weak`. View destruction fires
// `IView::on_destroy` while the NSView* is still valid, so
// BindingEngine can drop its per-view subscription bucket cleanly.

#import "aria/adapters/appkit/AppKitAdapter.hpp"
#import "aria/adapters/appkit/AppKitTableSource.hpp"

#include "aria/abi/signal.hpp"
#include "aria/abi/slot_factory.hpp"
#include "aria/binding/detail/numeric_saturate.hpp"
#include "aria/runtime/logger.hpp"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

// ═══════════════════════════════════════════════════════════════════════
//  ObjC bridging targets / delegates
// ═══════════════════════════════════════════════════════════════════════

@implementation AriaClickTarget {
    std::function<void()> _cb;
}
- (instancetype)initWithCallback:(std::function<void()>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    (void)sender;
    if (_cb) _cb();
}
// Backwards compatibility for clients that wired the no-argument selector.
- (void)fire {
    if (_cb) _cb();
}
@end

@implementation AriaToggleTarget {
    std::function<void(bool)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(bool)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    NSButton* b = (NSButton*)sender;
    if (_cb) _cb(b.state == NSControlStateValueOn);
}
@end

@implementation AriaStepperTarget {
    std::function<void(int)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(int)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    NSStepper* s = (NSStepper*)sender;
    if (_cb) _cb((int)s.intValue);
}
@end

@implementation AriaSliderTarget {
    std::function<void(double)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(double)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    NSSlider* s = (NSSlider*)sender;
    if (_cb) _cb(s.doubleValue);
}
@end

@implementation AriaTextDelegate {
    std::function<void(std::string_view)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(std::string_view)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)controlTextDidChange:(NSNotification*)note {
    NSTextField* tf = (NSTextField*)note.object;
    NSString* ns = tf.stringValue;
    const char* utf8 = ns.UTF8String;
    std::string_view sv(utf8, std::strlen(utf8));
    if (_cb) _cb(sv);
}
@end

// ═══════════════════════════════════════════════════════════════════════
//  AriaTableDataSource — NSTableViewDataSource + NSTableViewDelegate
//  bridge used by `aria::adapters::appkit::ObservableTableSource<T>`.
//
//  This is a non-template ObjC class. The C++ template wrapper feeds
//  it two `std::function`s captured by value; their lifetimes are
//  tied to the wrapper via a `weak_ptr<State>` (see
//  AppKitTableSource.hpp). The class implements the bare minimum
//  surface NSTableView requires:
//
//    - numberOfRowsInTableView:                 (DataSource)
//    - tableView:viewForTableColumn:row:        (Delegate)
//
//  We deliberately ignore selection / drag-and-drop / edit; consumers
//  who need those should subclass / extend the data source directly
//  on the application side. The C++ bridge fully owns the row count
//  and view supply, so AppKit never goes around us.
// ═══════════════════════════════════════════════════════════════════════

@implementation AriaTableDataSource {
    std::function<NSInteger()> _rowCountFn;
    std::function<NSView*(NSTableView*, NSTableColumn*, NSInteger)> _viewForFn;
}

- (instancetype)initWithRowCount:(std::function<NSInteger()>)rowCountFn
                       viewForFn:(std::function<NSView*(NSTableView*,
                                                        NSTableColumn*,
                                                        NSInteger)>)viewForFn {
    if ((self = [super init])) {
        _rowCountFn = std::move(rowCountFn);
        _viewForFn  = std::move(viewForFn);
    }
    return self;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    (void)tableView;
    return _rowCountFn ? _rowCountFn() : 0;
}

- (NSView*)tableView:(NSTableView*)tableView
  viewForTableColumn:(NSTableColumn*)tableColumn
                 row:(NSInteger)row {
    if (!_viewForFn) return nil;
    return _viewForFn(tableView, tableColumn, row);
}

@end

// ═══════════════════════════════════════════════════════════════════════
//  Adapter impl (pimpl)
// ═══════════════════════════════════════════════════════════════════════

namespace aria::adapters::appkit {

namespace {

// Slot args bag — what we pass through SignalErased::emit.
struct StringArgs { std::string_view sv; };
struct BoolArgs   { bool v; };
struct IntArgs    { int v; };
struct DoubleArgs { double v; };
struct VoidArgs   {};

// Build a SlotErased that owns the callable on the heap. Thin alias
// over the canonical factory in <aria/abi/slot_factory.hpp> so the
// call sites in this file stay readable as `make_slot([](void*){...})`.
template<typename Fn>
::aria::abi::SlotErased make_slot(Fn&& fn) {
    return ::aria::abi::make_slot_erased(std::forward<Fn>(fn));
}

// Per-(view, kind) bridge. Owns the SignalErased and the ObjC target/
// delegate that fans the native event into the signal.
struct Bridge {
    ::aria::abi::SignalErased sig;
    id __strong target = nil;     // AriaXxxTarget / AriaTextDelegate
};

NSView* native_of(::aria::binding::IView& v) {
    auto* av = dynamic_cast<AppKitView*>(&v);
    return av ? av->native() : nil;
}

// Emit one warning line when a binding op is asked to act on a NSView*
// whose class isn't covered by this adapter. Same shape as the Qt6
// adapter's warn_unsupported so log filters can match either platform
// with one regex (`adapter: no binding path for ...`).
void warn_unsupported_(const char* op, NSView* o) {
    auto& log = ::aria::runtime::Logger::instance();
    const char* cls = o ? object_getClassName(o) : "<null>";
    std::string msg;
    msg.reserve(64);
    msg.append(op).append(": no binding path for view class '").append(cls).append("'");
    log.warn("appkit_adapter", msg);
}

}  // namespace

struct AppKitAdapter::Impl {
    struct Key {
        const void* v;   // __bridge from NSView*; opaque identity only
        char        k;
        bool operator==(const Key& o) const noexcept { return v == o.v && k == o.k; }
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            // See qt_adapter.cpp's KeyHash for the rationale: char k
            // has only ~5 distinct values, so a plain XOR distributes
            // poorly. Mix via the boost::hash_combine constant so
            // collisions stay below the unordered_map load factor.
            size_t h = std::hash<const void*>{}(key.v);
            h ^= static_cast<size_t>(static_cast<unsigned char>(key.k))
                 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::mutex                                                                mu;
    std::unordered_map<Key, std::unique_ptr<Bridge>, KeyHash>                 bridges;
    // Per-view destroy subscription so we can drop bridges when the
    // C++ view wrapper goes away. Keyed on the same opaque void*.
    std::unordered_map<const void*, std::vector<::aria::Subscription>>        destroy_subs;
    // Handle → IView cache backing `view_for`. One AppKitView per NSView, so
    // repeated `view_for(control)` calls share a single per-view
    // subscription bucket inside BindingEngine.
    std::unordered_map<const void*, std::unique_ptr<AppKitView>>              views;

    ~Impl() {
        // The cached AppKitViews are moved out and destroyed AFTER the lock
        // is released: ~AppKitView fires IView::on_destroy, whose handlers
        // include our own `bridges.erase` lambda (which relocks `mu`) plus
        // BindingEngine's bucket teardown and arbitrary user callbacks.
        // Destroying them under the lock would self-deadlock on the first
        // bridge handler.
        decltype(views) doomed;
        {
            std::lock_guard lk{mu};
            doomed.swap(views);
        }
        doomed.clear();
        std::lock_guard lk{mu};
        bridges.clear();
        destroy_subs.clear();
    }

    // Destroy a cached view outside the lock, for the same reason as ~Impl.
    // Returns the extracted owner so the caller controls the destruction
    // point; discarding the return value destroys it at end of statement,
    // which is already outside any lock held by the caller.
    [[nodiscard]] std::unique_ptr<AppKitView> extract_view(const void* key) {
        std::lock_guard lk{mu};
        auto it = views.find(key);
        if (it == views.end()) return nullptr;
        auto owned = std::move(it->second);
        views.erase(it);
        return owned;
    }

    template<class WireFn>
    Bridge& bridge_for(::aria::binding::IView& view, NSView* obj, char kind, WireFn&& wire) {
        const void* key_ptr = (__bridge const void*)obj;
        std::lock_guard lk{mu};
        auto it = bridges.find(Key{key_ptr, kind});
        if (it != bridges.end()) return *it->second;

        auto br = std::make_unique<Bridge>();
        Bridge* raw = br.get();
        wire(*raw);
        bridges.emplace(Key{key_ptr, kind}, std::move(br));

        auto& subs = destroy_subs[key_ptr];
        subs.push_back(view.on_destroy([this, k = Key{key_ptr, kind}]() {
            std::lock_guard lk2{mu};
            bridges.erase(k);
        }));
        return *raw;
    }
};

AppKitAdapter::AppKitAdapter() : p_(std::make_unique<Impl>()) {}
AppKitAdapter::~AppKitAdapter() = default;

// ── view_for / release_view ────────────────────────────────────────────

AppKitView& AppKitAdapter::view_for(NSView* view) {
    if (!view)
        throw std::invalid_argument("AppKitAdapter::view_for: view must not be nil");

    const void* key = (__bridge const void*)view;
    std::lock_guard lk{p_->mu};
    auto it = p_->views.find(key);
    if (it != p_->views.end()) return *it->second;

    auto owned = std::make_unique<AppKitView>(view);
    AppKitView* raw = owned.get();
    p_->views.emplace(key, std::move(owned));
    return *raw;
}

void AppKitAdapter::release_view(NSView* view) noexcept {
    if (!view) return;
    // Extract under the lock, destroy after it is released: ~AppKitView
    // fires on_destroy, which reaches our own bridge cleanup (relocking
    // `mu`) and then user callbacks.
    auto doomed = p_->extract_view((__bridge const void*)view);
    (void)doomed;
}

// ── Text ───────────────────────────────────────────────────────────────

void AppKitAdapter::set_text(::aria::binding::IView& v, std::string_view text) {
    NSView* o = native_of(v); if (!o) return;
    NSString* ns = [[NSString alloc] initWithBytes:text.data()
                                            length:text.size()
                                          encoding:NSUTF8StringEncoding];
    if ([o isKindOfClass:[NSTextField class]]) {
        ((NSTextField*)o).stringValue = ns ?: @"";
    } else if ([o isKindOfClass:[NSPopUpButton class]]) {
        [((NSPopUpButton*)o) selectItemWithTitle:(ns ?: @"")];
    } else if ([o isKindOfClass:[NSButton class]]) {
        ((NSButton*)o).title = ns ?: @"";
    } else {
        warn_unsupported_("set_text", o);
    }
}

std::string AppKitAdapter::get_text(::aria::binding::IView& v) {
    NSView* o = native_of(v); if (!o) return {};
    NSString* ns = nil;
    if ([o isKindOfClass:[NSTextField class]]) {
        ns = ((NSTextField*)o).stringValue;
    } else if ([o isKindOfClass:[NSPopUpButton class]]) {
        ns = ((NSPopUpButton*)o).titleOfSelectedItem;
    } else if ([o isKindOfClass:[NSButton class]]) {
        ns = ((NSButton*)o).title;
    } else {
        warn_unsupported_("get_text", o);
        return {};
    }
    const char* utf8 = ns.UTF8String;
    return utf8 ? std::string(utf8) : std::string{};
}

::aria::Subscription AppKitAdapter::on_text_changed(::aria::binding::IView& v,
        std::function<void(std::string_view)> cb) {
    NSView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[NSTextField class]]) { warn_unsupported_("on_text_changed", o); return {}; }
    NSTextField* tf = (NSTextField*)o;

    auto& br = p_->bridge_for(v, o, 't', [tf](Bridge& bridge) {
        AriaTextDelegate* d = [[AriaTextDelegate alloc]
            initWithCallback:[bp = &bridge](std::string_view sv) {
                StringArgs a{sv};
                bp->sig.emit(&a);
            }];
        bridge.target = d;
        tf.delegate   = d;
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<StringArgs*>(args)->sv);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Bool ───────────────────────────────────────────────────────────────

void AppKitAdapter::set_bool(::aria::binding::IView& v, bool value) {
    NSView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[NSButton class]]) {
        ((NSButton*)o).state = value ? NSControlStateValueOn : NSControlStateValueOff;
    } else {
        warn_unsupported_("set_bool", o);
    }
}

bool AppKitAdapter::get_bool(::aria::binding::IView& v) {
    NSView* o = native_of(v); if (!o) return false;
    if ([o isKindOfClass:[NSButton class]]) {
        return ((NSButton*)o).state == NSControlStateValueOn;
    }
    warn_unsupported_("get_bool", o);
    return false;
}

::aria::Subscription AppKitAdapter::on_bool_changed(::aria::binding::IView& v,
        std::function<void(bool)> cb) {
    NSView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[NSButton class]]) { warn_unsupported_("on_bool_changed", o); return {}; }
    NSButton* btn = (NSButton*)o;

    auto& br = p_->bridge_for(v, o, 'b', [btn](Bridge& bridge) {
        AriaToggleTarget* t = [[AriaToggleTarget alloc]
            initWithCallback:[bp = &bridge](bool x) {
                BoolArgs a{x};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        btn.target    = t;
        btn.action    = @selector(fire:);
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<BoolArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Int ────────────────────────────────────────────────────────────────

void AppKitAdapter::set_int(::aria::binding::IView& v, int value) {
    NSView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[NSStepper class]]) {
        ((NSStepper*)o).intValue = value;
    } else if ([o isKindOfClass:[NSSlider class]]) {
        ((NSSlider*)o).intValue = value;
    } else if ([o isKindOfClass:[NSControl class]]) {
        ((NSControl*)o).intValue = value;
    } else {
        warn_unsupported_("set_int", o);
    }
}

int AppKitAdapter::get_int(::aria::binding::IView& v) {
    NSView* o = native_of(v); if (!o) return 0;
    if ([o isKindOfClass:[NSControl class]]) {
        return (int)((NSControl*)o).intValue;
    }
    warn_unsupported_("get_int", o);
    return 0;
}

::aria::Subscription AppKitAdapter::on_int_changed(::aria::binding::IView& v,
        std::function<void(int)> cb) {
    NSView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[NSControl class]]) { warn_unsupported_("on_int_changed", o); return {}; }
    NSControl* ctl = (NSControl*)o;

    auto& br = p_->bridge_for(v, o, 'i', [ctl](Bridge& bridge) {
        AriaStepperTarget* t = [[AriaStepperTarget alloc]
            initWithCallback:[bp = &bridge](int x) {
                IntArgs a{x};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        ctl.target    = t;
        ctl.action    = @selector(fire:);
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<IntArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

void AppKitAdapter::set_int64(::aria::binding::IView& v, std::int64_t value) {
    set_int(v, ::aria::binding::detail::saturate_int64_to_int(value, "appkit::set_int64"));
}
std::int64_t AppKitAdapter::get_int64(::aria::binding::IView& v) {
    return static_cast<std::int64_t>(get_int(v));
}
::aria::Subscription AppKitAdapter::on_int64_changed(::aria::binding::IView& v,
        std::function<void(std::int64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) {
        cb(static_cast<std::int64_t>(x));
    });
}

void AppKitAdapter::set_uint64(::aria::binding::IView& v, std::uint64_t value) {
    set_int(v, ::aria::binding::detail::saturate_uint64_to_int(value, "appkit::set_uint64"));
}
std::uint64_t AppKitAdapter::get_uint64(::aria::binding::IView& v) {
    return ::aria::binding::detail::int_to_uint64_clamped(get_int(v));
}
::aria::Subscription AppKitAdapter::on_uint64_changed(::aria::binding::IView& v,
        std::function<void(std::uint64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) {
        cb(::aria::binding::detail::int_to_uint64_clamped(x));
    });
}

void AppKitAdapter::set_float(::aria::binding::IView& v, float value) {
    set_double(v, static_cast<double>(value));
}
float AppKitAdapter::get_float(::aria::binding::IView& v) {
    return static_cast<float>(get_double(v));
}
::aria::Subscription AppKitAdapter::on_float_changed(::aria::binding::IView& v,
        std::function<void(float)> cb) {
    return on_double_changed(v, [cb = std::move(cb)](double x) {
        cb(static_cast<float>(x));
    });
}

// ── Double ─────────────────────────────────────────────────────────────

void AppKitAdapter::set_double(::aria::binding::IView& v, double value) {
    NSView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[NSSlider class]]) {
        ((NSSlider*)o).doubleValue = value;
    } else if ([o isKindOfClass:[NSControl class]]) {
        ((NSControl*)o).doubleValue = value;
    } else {
        warn_unsupported_("set_double", o);
    }
}

double AppKitAdapter::get_double(::aria::binding::IView& v) {
    NSView* o = native_of(v); if (!o) return 0.0;
    if ([o isKindOfClass:[NSControl class]]) {
        return ((NSControl*)o).doubleValue;
    }
    warn_unsupported_("get_double", o);
    return 0.0;
}

::aria::Subscription AppKitAdapter::on_double_changed(::aria::binding::IView& v,
        std::function<void(double)> cb) {
    NSView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[NSControl class]]) { warn_unsupported_("on_double_changed", o); return {}; }
    NSControl* ctl = (NSControl*)o;

    auto& br = p_->bridge_for(v, o, 'd', [ctl](Bridge& bridge) {
        AriaSliderTarget* t = [[AriaSliderTarget alloc]
            initWithCallback:[bp = &bridge](double x) {
                DoubleArgs a{x};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        ctl.target    = t;
        ctl.action    = @selector(fire:);
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<DoubleArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Visibility / enabled ───────────────────────────────────────────────

void AppKitAdapter::set_visible(::aria::binding::IView& v, bool visible) {
    NSView* o = native_of(v); if (!o) return;
    o.hidden = !visible;
}

void AppKitAdapter::set_enabled(::aria::binding::IView& v, bool enabled) {
    NSView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[NSControl class]]) {
        ((NSControl*)o).enabled = enabled;
    } else {
        warn_unsupported_("set_enabled", o);
    }
}

// ── Click ──────────────────────────────────────────────────────────────

::aria::Subscription AppKitAdapter::on_click(::aria::binding::IView& v,
        std::function<void()> cb) {
    NSView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[NSButton class]]) { warn_unsupported_("on_click", o); return {}; }
    NSButton* btn = (NSButton*)o;

    auto& br = p_->bridge_for(v, o, 'c', [btn](Bridge& bridge) {
        AriaClickTarget* t = [[AriaClickTarget alloc]
            initWithCallback:[bp = &bridge]() {
                VoidArgs a{};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        btn.target    = t;
        btn.action    = @selector(fire:);
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* /*args*/) {
        cb();
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

}  // namespace aria::adapters::appkit
