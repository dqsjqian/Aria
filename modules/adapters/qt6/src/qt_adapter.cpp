#include "aria/adapters/qt6/qt_adapter.hpp"
#include "aria/abi/signal.hpp"
#include "aria/abi/slot_factory.hpp"
#include "aria/runtime/logger.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTextEdit>
#include <QWidget>

#include "aria/binding/detail/numeric_saturate.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aria::adapters::qt6 {

namespace {

// Slot args bag — what we pass through SignalErased::emit.
struct StringArgs { std::string_view sv; };
struct BoolArgs   { bool v; };
struct IntArgs    { int v; };
struct DoubleArgs { double v; };
struct VoidArgs   {};

// Build a SlotErased that owns a std::function<void(void*)> on the heap,
// invokes it with the raw args pointer. Thin alias over the canonical
// factory in <aria/abi/slot_factory.hpp> -- kept so the call sites in
// this file stay readable as `make_slot([this, ...](void* a){ ... })`.
template<typename Fn>
abi::SlotErased make_slot(Fn&& fn) {
    return abi::make_slot_erased(std::forward<Fn>(fn));
}

// Resolve QObject from IView (must be a QtView).
QObject* obj_of(binding::IView& v) {
    auto* qv = dynamic_cast<QtView*>(&v);
    return qv ? qv->object() : nullptr;
}

// Emit a single warning line telling the user their widget class isn't
// supported by the operation they invoked.  We keep it cheap — adapters
// are hot paths so this only fires on the "wrong type" branch.
void warn_unsupported(const char* op, QObject* o) {
    auto& log = runtime::Logger::instance();
    const char* cls = o && o->metaObject() ? o->metaObject()->className()
                                            : "<null>";
    std::string msg;
    msg.reserve(64);
    msg.append(op).append(": no binding path for widget class '").append(cls).append("'");
    log.warn("qt_adapter", msg);
}

// A small helper that owns ONE SignalErased and bridges Qt signals into it.
// We cache one of these per (QObject*, slot-kind) so multiple subscribers
// share the same Qt::connect.
struct Bridge {
    abi::SignalErased sig;
    QMetaObject::Connection qt_conn;
    QMetaObject::Connection destroyed_conn;
};

}  // namespace

struct QtAdapter::Impl {
    // Map key = (object, kind). kind = 't' text, 'b' bool, 'i' int,
    // 'd' double, 'c' click.
    struct Key {
        QObject* o;
        char k;
        bool operator==(const Key& other) const noexcept {
            return o == other.o && k == other.k;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            // Boost-style hash combine: simple XOR with a tiny
            // shifted-char gives a poor distribution because most of
            // the entropy in `key.k` (only 5 distinct values) lands in
            // a single bit. Mixing through the magic constant +
            // self-shifts spreads it into the high bits, which the
            // unordered_map's truncation modulo prime then samples.
            size_t h = std::hash<QObject*>{}(key.o);
            h ^= static_cast<size_t>(static_cast<unsigned char>(key.k))
                 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::mutex m;
    std::unordered_map<Key, std::unique_ptr<Bridge>, KeyHash> bridges;

    ~Impl() {
        // Drop our QObject::destroyed listeners so a widget that outlives
        // *this can't try to call our `bridges.erase(...)` lambda after the
        // mutex is gone.
        std::lock_guard lk{m};
        for (auto& [_, br] : bridges) {
            QObject::disconnect(br->destroyed_conn);
            QObject::disconnect(br->qt_conn);
        }
        bridges.clear();
    }

    Bridge& bridge_for(QObject* obj, char kind, auto&& wire_qt_signal) {
        std::lock_guard lk{m};
        auto it = bridges.find(Key{obj, kind});
        if (it != bridges.end()) return *it->second;

        auto br = std::make_unique<Bridge>();
        Bridge* raw = br.get();
        // wire the Qt signal once
        wire_qt_signal(*raw);
        // also drop the bridge if the QObject dies
        raw->destroyed_conn = QObject::connect(obj, &QObject::destroyed,
            [this, k = Key{obj, kind}]() {
                std::lock_guard lk2{m};
                bridges.erase(k);
            });
        bridges.emplace(Key{obj, kind}, std::move(br));
        return *raw;
    }
};

QtAdapter::QtAdapter() : p_(std::make_unique<Impl>()) {}
QtAdapter::~QtAdapter() = default;

// ── Text ────────────────────────────────────────────────────────────────────
void QtAdapter::set_text(binding::IView& v, std::string_view text) {
    auto* o = obj_of(v); if (!o) return;
    if (auto* le = qobject_cast<QLineEdit*>(o))      le->setText(QString::fromUtf8(text.data(), int(text.size())));
    else if (auto* lb = qobject_cast<QLabel*>(o))    lb->setText(QString::fromUtf8(text.data(), int(text.size())));
    else if (auto* pe = qobject_cast<QPlainTextEdit*>(o)) pe->setPlainText(QString::fromUtf8(text.data(), int(text.size())));
    else if (auto* te = qobject_cast<QTextEdit*>(o)) te->setPlainText(QString::fromUtf8(text.data(), int(text.size())));
    else if (auto* cb = qobject_cast<QComboBox*>(o)) cb->setCurrentText(QString::fromUtf8(text.data(), int(text.size())));
    else if (auto* ab = qobject_cast<QAbstractButton*>(o)) ab->setText(QString::fromUtf8(text.data(), int(text.size())));
    else                                             warn_unsupported("set_text", o);
}

std::string QtAdapter::get_text(binding::IView& v) {
    auto* o = obj_of(v); if (!o) return {};
    QString s;
    if (auto* le = qobject_cast<QLineEdit*>(o))      s = le->text();
    else if (auto* lb = qobject_cast<QLabel*>(o))    s = lb->text();
    else if (auto* pe = qobject_cast<QPlainTextEdit*>(o)) s = pe->toPlainText();
    else if (auto* te = qobject_cast<QTextEdit*>(o)) s = te->toPlainText();
    else if (auto* cb = qobject_cast<QComboBox*>(o)) s = cb->currentText();
    else if (auto* ab = qobject_cast<QAbstractButton*>(o)) s = ab->text();
    else                                           { warn_unsupported("get_text", o); return {}; }
    auto utf8 = s.toUtf8();
    return std::string(utf8.constData(), size_t(utf8.size()));
}

::aria::Subscription QtAdapter::on_text_changed(binding::IView& v,
                                            std::function<void(std::string_view)> cb) {
    auto* o = obj_of(v); if (!o) return {};
    if (!qobject_cast<QLineEdit*>(o) && !qobject_cast<QPlainTextEdit*>(o) &&
        !qobject_cast<QTextEdit*>(o) && !qobject_cast<QComboBox*>(o)) {
        warn_unsupported("on_text_changed", o);
        return {};
    }
    auto& br = p_->bridge_for(o, 't', [o](Bridge& bridge) {
        auto fwd = [b = &bridge](const QString& s) {
            auto utf8 = s.toUtf8();
            StringArgs a{std::string_view(utf8.constData(), size_t(utf8.size()))};
            b->sig.emit(&a);
        };
        if (auto* le = qobject_cast<QLineEdit*>(o))
            bridge.qt_conn = QObject::connect(le, &QLineEdit::textChanged, fwd);
        else if (auto* cb = qobject_cast<QComboBox*>(o))
            bridge.qt_conn = QObject::connect(cb, &QComboBox::currentTextChanged, fwd);
        else if (auto* pe = qobject_cast<QPlainTextEdit*>(o))
            bridge.qt_conn = QObject::connect(pe, &QPlainTextEdit::textChanged,
                [pe, b = &bridge]{
                    auto utf8 = pe->toPlainText().toUtf8();
                    StringArgs a{std::string_view(utf8.constData(), size_t(utf8.size()))};
                    b->sig.emit(&a);
                });
        else if (auto* te = qobject_cast<QTextEdit*>(o))
            bridge.qt_conn = QObject::connect(te, &QTextEdit::textChanged,
                [te, b = &bridge]{
                    auto utf8 = te->toPlainText().toUtf8();
                    StringArgs a{std::string_view(utf8.constData(), size_t(utf8.size()))};
                    b->sig.emit(&a);
                });
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<StringArgs*>(args)->sv);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Bool ────────────────────────────────────────────────────────────────────
void QtAdapter::set_bool(binding::IView& v, bool value) {
    auto* o = obj_of(v); if (!o) return;
    if (auto* b = qobject_cast<QAbstractButton*>(o)) b->setChecked(value);
    else                                             warn_unsupported("set_bool", o);
}

bool QtAdapter::get_bool(binding::IView& v) {
    auto* o = obj_of(v); if (!o) return false;
    if (auto* b = qobject_cast<QAbstractButton*>(o)) return b->isChecked();
    warn_unsupported("get_bool", o);
    return false;
}

::aria::Subscription QtAdapter::on_bool_changed(binding::IView& v,
                                            std::function<void(bool)> cb) {
    auto* o = obj_of(v); if (!o) return {};
    if (!qobject_cast<QAbstractButton*>(o)) { warn_unsupported("on_bool_changed", o); return {}; }
    auto& br = p_->bridge_for(o, 'b', [o](Bridge& bridge) {
        if (auto* b = qobject_cast<QAbstractButton*>(o))
            bridge.qt_conn = QObject::connect(b, &QAbstractButton::toggled,
                [bp = &bridge](bool x) { BoolArgs a{x}; bp->sig.emit(&a); });
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<BoolArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Int ─────────────────────────────────────────────────────────────────────
void QtAdapter::set_int(binding::IView& v, int value) {
    auto* o = obj_of(v); if (!o) return;
    if (auto* sp = qobject_cast<QSpinBox*>(o))          sp->setValue(value);
    else if (auto* sl = qobject_cast<QSlider*>(o))      sl->setValue(value);
    else if (auto* pb = qobject_cast<QProgressBar*>(o)) pb->setValue(value);
    else                                                warn_unsupported("set_int", o);
}

int QtAdapter::get_int(binding::IView& v) {
    auto* o = obj_of(v); if (!o) return 0;
    if (auto* sp = qobject_cast<QSpinBox*>(o))          return sp->value();
    if (auto* sl = qobject_cast<QSlider*>(o))           return sl->value();
    if (auto* pb = qobject_cast<QProgressBar*>(o))      return pb->value();
    warn_unsupported("get_int", o);
    return 0;
}

::aria::Subscription QtAdapter::on_int_changed(binding::IView& v,
                                           std::function<void(int)> cb) {
    auto* o = obj_of(v); if (!o) return {};
    if (!qobject_cast<QSpinBox*>(o) && !qobject_cast<QSlider*>(o)) {
        warn_unsupported("on_int_changed", o);
        return {};
    }
    auto& br = p_->bridge_for(o, 'i', [o](Bridge& bridge) {
        auto fwd = [b = &bridge](int x) { IntArgs a{x}; b->sig.emit(&a); };
        if (auto* sp = qobject_cast<QSpinBox*>(o))
            bridge.qt_conn = QObject::connect(sp, &QSpinBox::valueChanged, fwd);
        else if (auto* sl = qobject_cast<QSlider*>(o))
            bridge.qt_conn = QObject::connect(sl, &QSlider::valueChanged, fwd);
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<IntArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Double ──────────────────────────────────────────────────────────────────
void QtAdapter::set_double(binding::IView& v, double value) {
    auto* o = obj_of(v); if (!o) return;
    if (auto* s = qobject_cast<QDoubleSpinBox*>(o)) s->setValue(value);
    else                                            warn_unsupported("set_double", o);
}

double QtAdapter::get_double(binding::IView& v) {
    auto* o = obj_of(v); if (!o) return 0.0;
    if (auto* s = qobject_cast<QDoubleSpinBox*>(o)) return s->value();
    warn_unsupported("get_double", o);
    return 0.0;
}

::aria::Subscription QtAdapter::on_double_changed(binding::IView& v,
                                              std::function<void(double)> cb) {
    auto* o = obj_of(v); if (!o) return {};
    if (!qobject_cast<QDoubleSpinBox*>(o)) { warn_unsupported("on_double_changed", o); return {}; }
    auto& br = p_->bridge_for(o, 'd', [o](Bridge& bridge) {
        if (auto* s = qobject_cast<QDoubleSpinBox*>(o))
            bridge.qt_conn = QObject::connect(s, &QDoubleSpinBox::valueChanged,
                [b = &bridge](double x) { DoubleArgs a{x}; b->sig.emit(&a); });
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* args) {
        cb(static_cast<DoubleArgs*>(args)->v);
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

// ── Int64 / UInt64 / Float ───────────────────────────────────────────────────
//
// Qt's numeric widgets top out at int / double. We forward these
// wider/narrower overloads through the matching int or double path, so users
// who happen to use Property<int64_t> / Property<uint64_t> / Property<float>
// against a QSpinBox / QDoubleSpinBox still work. Applications that genuinely
// need the full 64-bit range should drive a QLineEdit via bind_text_converted.
void QtAdapter::set_int64(binding::IView& v, std::int64_t value) {
    set_int(v, ::aria::binding::detail::saturate_int64_to_int(value, "qt::set_int64"));
}
std::int64_t QtAdapter::get_int64(binding::IView& v) {
    return static_cast<std::int64_t>(get_int(v));
}
::aria::Subscription QtAdapter::on_int64_changed(binding::IView& v,
                                             std::function<void(std::int64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) {
        cb(static_cast<std::int64_t>(x));
    });
}

void QtAdapter::set_uint64(binding::IView& v, std::uint64_t value) {
    set_int(v, ::aria::binding::detail::saturate_uint64_to_int(value, "qt::set_uint64"));
}
std::uint64_t QtAdapter::get_uint64(binding::IView& v) {
    return ::aria::binding::detail::int_to_uint64_clamped(get_int(v));
}
::aria::Subscription QtAdapter::on_uint64_changed(binding::IView& v,
                                              std::function<void(std::uint64_t)> cb) {
    return on_int_changed(v, [cb = std::move(cb)](int x) {
        cb(::aria::binding::detail::int_to_uint64_clamped(x));
    });
}

void QtAdapter::set_float(binding::IView& v, float value) {
    set_double(v, static_cast<double>(value));
}
float QtAdapter::get_float(binding::IView& v) {
    return static_cast<float>(get_double(v));
}
::aria::Subscription QtAdapter::on_float_changed(binding::IView& v,
                                             std::function<void(float)> cb) {
    return on_double_changed(v, [cb = std::move(cb)](double x) {
        cb(static_cast<float>(x));
    });
}

// ── Visibility / enabled ──────────────────────────────────────────────
void QtAdapter::set_visible(binding::IView& v, bool visible) {
    auto* o = obj_of(v); if (!o) return;
    if (auto* w = qobject_cast<QWidget*>(o)) w->setVisible(visible);
    else                                     warn_unsupported("set_visible", o);
}

void QtAdapter::set_enabled(binding::IView& v, bool enabled) {
    auto* o = obj_of(v); if (!o) return;
    if (auto* w = qobject_cast<QWidget*>(o)) w->setEnabled(enabled);
    else                                     warn_unsupported("set_enabled", o);
}
// ── Click ───────────────────────────────────────────────────────────────────
::aria::Subscription QtAdapter::on_click(binding::IView& v,
                                     std::function<void()> cb) {
    auto* o = obj_of(v); if (!o) return {};
    if (!qobject_cast<QAbstractButton*>(o)) {
        warn_unsupported("on_click", o);
        return {};
    }
    auto& br = p_->bridge_for(o, 'c', [o](Bridge& bridge) {
        if (auto* b = qobject_cast<QAbstractButton*>(o))
            bridge.qt_conn = QObject::connect(b, &QAbstractButton::clicked,
                [bp = &bridge](bool /*checked*/){ VoidArgs a{}; bp->sig.emit(&a); });
    });
    auto id = br.sig.connect(make_slot([cb = std::move(cb)](void* /*args*/) {
        cb();
    }));
    auto weak = br.sig.weak_handle();
    return ::aria::Subscription{[weak, id]() noexcept {
        abi::SignalErased::disconnect_via_weak(weak, id);
    }};
}

}  // namespace aria::adapters::qt6
