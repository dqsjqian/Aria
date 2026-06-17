// UIKitAdapter.mm — production-grade IViewAdapter for iOS UIKit.
//
// Mirrors AppKitAdapter / Qt6 adapter: per-(view, kind) Bridge cache
// holding a SignalErased + an ObjC target that fans the native event
// into the signal. Subscriptions returned from on_*_changed / on_click
// detach properly via SignalErased::disconnect_via_weak.

#import "UIKitAdapter.hpp"
#import "UIKitTableSource.hpp"

#include "aria/abi/signal.hpp"
#include "aria/abi/slot_factory.hpp"
#include "aria/binding/detail/numeric_saturate.hpp"
#include "aria/runtime/logger.hpp"
#include "aria/reactive/reactive.hpp"  // pulls graph.inl Node defs

#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <cstring>

// ═══════════════════════════════════════════════════════════════════════
//  ObjC bridging targets
// ═══════════════════════════════════════════════════════════════════════

@implementation AriaUIClickTarget {
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
// Backwards-compat: legacy examples (demo3 RootViewController) wired
// `[btn addTarget:wrapper action:@selector(fire) ...]`. Keep that
// zero-arg selector working so existing example code does not crash
// with `unrecognized selector`.
- (void)fire {
    if (_cb) _cb();
}
@end

@implementation AriaUIToggleTarget {
    std::function<void(bool)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(bool)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    UISwitch* s = (UISwitch*)sender;
    if (_cb) _cb(s.isOn);
}
@end

@implementation AriaUIStepperTarget {
    std::function<void(int)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(int)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    UIStepper* s = (UIStepper*)sender;
    if (_cb) _cb((int)s.value);
}
@end

@implementation AriaUISliderTarget {
    std::function<void(double)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(double)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    UISlider* s = (UISlider*)sender;
    if (_cb) _cb((double)s.value);
}
@end

@implementation AriaUITextTarget {
    std::function<void(std::string_view)> _cb;
}
- (instancetype)initWithCallback:(std::function<void(std::string_view)>)cb {
    if ((self = [super init])) { _cb = std::move(cb); }
    return self;
}
- (void)fire:(id)sender {
    UITextField* tf = (UITextField*)sender;
    NSString* ns = tf.text ?: @"";
    const char* utf8 = ns.UTF8String;
    std::string_view sv(utf8, std::strlen(utf8));
    if (_cb) _cb(sv);
}
@end

// ═══════════════════════════════════════════════════════════════════════
//  AriaUITableDataSource — UITableViewDataSource + UITableViewDelegate
//  bridge used by `aria::adapters::uikit::ObservableTableSource<T>`.
//
//  Mirrors AppKit's AriaTableDataSource. Implements the bare minimum
//  surface UITableView requires:
//
//    - tableView:numberOfRowsInSection:           (DataSource)
//    - tableView:cellForRowAtIndexPath:           (DataSource)
//
//  Selection / editing / drag-and-drop are intentionally out of
//  scope; consumers who need those should layer their own delegate
//  on top of the bridge.
// ═══════════════════════════════════════════════════════════════════════

@implementation AriaUITableDataSource {
    std::function<NSInteger()> _rowCountFn;
    std::function<UITableViewCell*(UITableView*, NSIndexPath*)> _cellForFn;
}

- (instancetype)initWithRowCount:(std::function<NSInteger()>)rowCountFn
                       cellForFn:(std::function<UITableViewCell*(UITableView*,
                                                                  NSIndexPath*)>)cellForFn {
    if ((self = [super init])) {
        _rowCountFn = std::move(rowCountFn);
        _cellForFn  = std::move(cellForFn);
    }
    return self;
}

- (NSInteger)tableView:(UITableView*)tableView
 numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    (void)section;
    return _rowCountFn ? _rowCountFn() : 0;
}

- (UITableViewCell*)tableView:(UITableView*)tableView
        cellForRowAtIndexPath:(NSIndexPath*)indexPath {
    if (!_cellForFn) {
        return [[UITableViewCell alloc]
                    initWithStyle:UITableViewCellStyleDefault
                  reuseIdentifier:@"empty"];
    }
    return _cellForFn(tableView, indexPath);
}

@end

// ═══════════════════════════════════════════════════════════════════════
//  Adapter impl (pimpl)
// ═══════════════════════════════════════════════════════════════════════

namespace aria::adapters::uikit {

namespace {

struct StringArgs { std::string_view sv; };
struct BoolArgs   { bool v; };
struct IntArgs    { int v; };
struct DoubleArgs { double v; };
struct VoidArgs   {};

// Thin alias over the canonical factory in <aria/abi/slot_factory.hpp>
// so call sites in this file stay readable as `make_slot([](void*){...})`.
template<typename Fn>
::aria::abi::SlotErased make_slot(Fn&& fn) {
    return ::aria::abi::make_slot_erased(std::forward<Fn>(fn));
}

struct Bridge {
    ::aria::abi::SignalErased sig;
    id __strong target = nil;
};

UIView* native_of(::aria::binding::IView& v) {
    auto* uv = dynamic_cast<UIKitView*>(&v);
    return uv ? uv->native() : nil;
}

// Emit one warning line when a binding op is asked to act on a UIView*
// whose class isn't covered by this adapter. Same shape as the Qt6 /
// AppKit adapter's warn_unsupported so log filters can match all three
// platforms with one regex (`adapter: no binding path for ...`).
void warn_unsupported_(const char* op, UIView* o) {
    auto& log = ::aria::runtime::Logger::instance();
    const char* cls = o ? object_getClassName(o) : "<null>";
    std::string msg;
    msg.reserve(64);
    msg.append(op).append(": no binding path for view class '").append(cls).append("'");
    log.warn("uikit_adapter", msg);
}

}  // namespace

struct UIKitAdapter::Impl {
    struct Key {
        const void* v;
        char        k;
        bool operator==(const Key& o) const noexcept { return v == o.v && k == o.k; }
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            // Mix the kind char with the pointer hash via the
            // boost::hash_combine magic constant. Plain XOR of a 5-value
            // shifted char with a pointer hash produces a high
            // collision rate; the hash_combine path scatters the kind
            // through the high bits.
            size_t h = std::hash<const void*>{}(key.v);
            h ^= static_cast<size_t>(static_cast<unsigned char>(key.k))
                 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::mutex                                                                mu;
    std::unordered_map<Key, std::unique_ptr<Bridge>, KeyHash>                 bridges;
    std::unordered_map<const void*, std::vector<::aria::Subscription>>        destroy_subs;

    ~Impl() {
        std::lock_guard lk{mu};
        bridges.clear();
        destroy_subs.clear();
    }

    template<class WireFn>
    Bridge& bridge_for(::aria::binding::IView& view, UIView* obj, char kind, WireFn&& wire) {
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

UIKitAdapter::UIKitAdapter() : p_(std::make_unique<Impl>()) {}
UIKitAdapter::~UIKitAdapter() = default;

// ── Text ───────────────────────────────────────────────────────────────

void UIKitAdapter::set_text(::aria::binding::IView& v, std::string_view text) {
    UIView* o = native_of(v); if (!o) return;
    NSString* ns = [[NSString alloc] initWithBytes:text.data()
                                            length:text.size()
                                          encoding:NSUTF8StringEncoding];
    if ([o isKindOfClass:[UITextField class]])      ((UITextField*)o).text = ns ?: @"";
    else if ([o isKindOfClass:[UILabel class]])     ((UILabel*)o).text     = ns ?: @"";
    else if ([o isKindOfClass:[UITextView class]])  ((UITextView*)o).text  = ns ?: @"";
    else                                            warn_unsupported_("set_text", o);
}

std::string UIKitAdapter::get_text(::aria::binding::IView& v) {
    UIView* o = native_of(v); if (!o) return {};
    NSString* ns = nil;
    if ([o isKindOfClass:[UITextField class]])      ns = ((UITextField*)o).text;
    else if ([o isKindOfClass:[UILabel class]])     ns = ((UILabel*)o).text;
    else if ([o isKindOfClass:[UITextView class]])  ns = ((UITextView*)o).text;
    else                                          { warn_unsupported_("get_text", o); return {}; }
    if (!ns) return {};
    const char* utf8 = ns.UTF8String;
    return utf8 ? std::string(utf8) : std::string{};
}

::aria::Subscription UIKitAdapter::on_text_changed(::aria::binding::IView& v,
        std::function<void(std::string_view)> cb) {
    UIView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[UITextField class]]) { warn_unsupported_("on_text_changed", o); return {}; }
    UITextField* tf = (UITextField*)o;

    auto& br = p_->bridge_for(v, o, 't', [tf](Bridge& bridge) {
        AriaUITextTarget* t = [[AriaUITextTarget alloc]
            initWithCallback:[bp = &bridge](std::string_view sv) {
                StringArgs a{sv};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        [tf addTarget:t
                action:@selector(fire:)
      forControlEvents:UIControlEventEditingChanged];
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<StringArgs*>(args)->sv);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Bool (UISwitch) ────────────────────────────────────────────────────

void UIKitAdapter::set_bool(::aria::binding::IView& v, bool value) {
    UIView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[UISwitch class]]) {
        [((UISwitch*)o) setOn:value animated:NO];
    } else {
        warn_unsupported_("set_bool", o);
    }
}

bool UIKitAdapter::get_bool(::aria::binding::IView& v) {
    UIView* o = native_of(v); if (!o) return false;
    if ([o isKindOfClass:[UISwitch class]]) return ((UISwitch*)o).isOn;
    warn_unsupported_("get_bool", o);
    return false;
}

::aria::Subscription UIKitAdapter::on_bool_changed(::aria::binding::IView& v,
        std::function<void(bool)> cb) {
    UIView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[UISwitch class]]) { warn_unsupported_("on_bool_changed", o); return {}; }
    UISwitch* sw = (UISwitch*)o;

    auto& br = p_->bridge_for(v, o, 'b', [sw](Bridge& bridge) {
        AriaUIToggleTarget* t = [[AriaUIToggleTarget alloc]
            initWithCallback:[bp = &bridge](bool x) {
                BoolArgs a{x};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        [sw addTarget:t
                action:@selector(fire:)
      forControlEvents:UIControlEventValueChanged];
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<BoolArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Int (UIStepper) ────────────────────────────────────────────────────

void UIKitAdapter::set_int(::aria::binding::IView& v, int value) {
    UIView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[UIStepper class]]) {
        ((UIStepper*)o).value = value;
    } else if ([o isKindOfClass:[UISlider class]]) {
        ((UISlider*)o).value = (float)value;
    } else {
        warn_unsupported_("set_int", o);
    }
}

int UIKitAdapter::get_int(::aria::binding::IView& v) {
    UIView* o = native_of(v); if (!o) return 0;
    if ([o isKindOfClass:[UIStepper class]])  return (int)((UIStepper*)o).value;
    if ([o isKindOfClass:[UISlider class]])   return (int)((UISlider*)o).value;
    warn_unsupported_("get_int", o);
    return 0;
}

::aria::Subscription UIKitAdapter::on_int_changed(::aria::binding::IView& v,
        std::function<void(int)> cb) {
    UIView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[UIControl class]]) { warn_unsupported_("on_int_changed", o); return {}; }
    UIControl* ctl = (UIControl*)o;

    auto& br = p_->bridge_for(v, o, 'i', [ctl](Bridge& bridge) {
        AriaUIStepperTarget* t = [[AriaUIStepperTarget alloc]
            initWithCallback:[bp = &bridge](int x) {
                IntArgs a{x};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        [ctl addTarget:t
                 action:@selector(fire:)
       forControlEvents:UIControlEventValueChanged];
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<IntArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

void UIKitAdapter::set_int64(::aria::binding::IView& v, std::int64_t value) {
    set_int(v, ::aria::binding::detail::saturate_int64_to_int(value, "uikit::set_int64"));
}
std::int64_t UIKitAdapter::get_int64(::aria::binding::IView& v) {
    return static_cast<std::int64_t>(get_int(v));
}
::aria::Subscription UIKitAdapter::on_int64_changed(::aria::binding::IView& v,
        std::function<void(std::int64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) {
        cb(static_cast<std::int64_t>(x));
    });
}

void UIKitAdapter::set_uint64(::aria::binding::IView& v, std::uint64_t value) {
    set_int(v, ::aria::binding::detail::saturate_uint64_to_int(value, "uikit::set_uint64"));
}
std::uint64_t UIKitAdapter::get_uint64(::aria::binding::IView& v) {
    return ::aria::binding::detail::int_to_uint64_clamped(get_int(v));
}
::aria::Subscription UIKitAdapter::on_uint64_changed(::aria::binding::IView& v,
        std::function<void(std::uint64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) {
        cb(::aria::binding::detail::int_to_uint64_clamped(x));
    });
}

void UIKitAdapter::set_float(::aria::binding::IView& v, float value) {
    set_double(v, static_cast<double>(value));
}
float UIKitAdapter::get_float(::aria::binding::IView& v) {
    return static_cast<float>(get_double(v));
}
::aria::Subscription UIKitAdapter::on_float_changed(::aria::binding::IView& v,
        std::function<void(float)> cb) {
    return on_double_changed(v, [cb = std::move(cb)](double x) {
        cb(static_cast<float>(x));
    });
}

// ── Double (UISlider) ──────────────────────────────────────────────────

void UIKitAdapter::set_double(::aria::binding::IView& v, double value) {
    UIView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[UISlider class]]) {
        ((UISlider*)o).value = (float)value;
    } else if ([o isKindOfClass:[UIStepper class]]) {
        ((UIStepper*)o).value = value;
    } else {
        warn_unsupported_("set_double", o);
    }
}

double UIKitAdapter::get_double(::aria::binding::IView& v) {
    UIView* o = native_of(v); if (!o) return 0.0;
    if ([o isKindOfClass:[UISlider class]])   return (double)((UISlider*)o).value;
    if ([o isKindOfClass:[UIStepper class]])  return ((UIStepper*)o).value;
    warn_unsupported_("get_double", o);
    return 0.0;
}

::aria::Subscription UIKitAdapter::on_double_changed(::aria::binding::IView& v,
        std::function<void(double)> cb) {
    UIView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[UIControl class]]) { warn_unsupported_("on_double_changed", o); return {}; }
    UIControl* ctl = (UIControl*)o;

    auto& br = p_->bridge_for(v, o, 'd', [ctl](Bridge& bridge) {
        AriaUISliderTarget* t = [[AriaUISliderTarget alloc]
            initWithCallback:[bp = &bridge](double x) {
                DoubleArgs a{x};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        [ctl addTarget:t
                 action:@selector(fire:)
       forControlEvents:UIControlEventValueChanged];
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

void UIKitAdapter::set_visible(::aria::binding::IView& v, bool visible) {
    UIView* o = native_of(v); if (!o) return;
    o.hidden = !visible;
}

void UIKitAdapter::set_enabled(::aria::binding::IView& v, bool enabled) {
    UIView* o = native_of(v); if (!o) return;
    if ([o isKindOfClass:[UIControl class]]) {
        ((UIControl*)o).enabled = enabled;
    } else {
        warn_unsupported_("set_enabled", o);
    }
}

// ── Click (UIButton) ───────────────────────────────────────────────────

::aria::Subscription UIKitAdapter::on_click(::aria::binding::IView& v,
        std::function<void()> cb) {
    UIView* o = native_of(v); if (!o) return {};
    if (![o isKindOfClass:[UIButton class]]) { warn_unsupported_("on_click", o); return {}; }
    UIButton* btn = (UIButton*)o;

    auto& br = p_->bridge_for(v, o, 'c', [btn](Bridge& bridge) {
        AriaUIClickTarget* t = [[AriaUIClickTarget alloc]
            initWithCallback:[bp = &bridge]() {
                VoidArgs a{};
                bp->sig.emit(&a);
            }];
        bridge.target = t;
        [btn addTarget:t
                 action:@selector(fire:)
       forControlEvents:UIControlEventTouchUpInside];
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* /*args*/) {
        cb();
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        ::aria::abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

}  // namespace aria::adapters::uikit
