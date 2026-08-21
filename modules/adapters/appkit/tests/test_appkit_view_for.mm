// test_appkit_view_for.mm — AppKitAdapter::view_for / release_view.
//
// Mirrors modules/adapters/qt6/tests/test_qt_view_for.cpp. Both adapters now
// own the handle → IView mapping that every host used to hand-roll (an
// `std::vector<std::unique_ptr<AppKitView>>` keepalive on the controller).
//
// The AppKit lifetime story differs from Qt's on purpose: `AppKitView`
// retains its `NSView*` via ARC, so there is no "native handle died first"
// event to evict on. Cached entries live until the adapter is destroyed or
// the host calls `release_view` — both paths are covered below.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#import "aria/adapters/appkit/AppKitAdapter.hpp"
#import "aria/binding/binding_engine.hpp"
#import "aria/reactive/reactive.hpp"

#import <Cocoa/Cocoa.h>

#include <memory>
#include <stdexcept>
#include <string>

using namespace aria;
using namespace aria::adapters::appkit;

namespace {

void ensure_nsapp() {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    });
}

}  // namespace

TEST_CASE("appkit view_for: returns the same AppKitView for the same NSView") {
    ensure_nsapp();
    @autoreleasepool {
        NSTextField* a = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 80, 22)];
        NSTextField* b = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 80, 22)];
        AppKitAdapter adapter;

        auto& va1 = adapter.view_for(a);
        auto& va2 = adapter.view_for(a);
        auto& vb  = adapter.view_for(b);

        CHECK(&va1 == &va2);          // cached — one AppKitView per handle
        CHECK(&va1 != &vb);
        // Compare bridged raw pointers: doctest stringifies its operands, and
        // it cannot take an ObjC pointer under ARC.
        CHECK((const void*)(__bridge const void*)va1.native()
              == (const void*)(__bridge const void*)a);
        CHECK(va1.kind() == "appkit");
    }
}

TEST_CASE("appkit view_for: rejects a nil handle") {
    ensure_nsapp();
    AppKitAdapter adapter;
    CHECK_THROWS_AS((void)adapter.view_for(nil), std::invalid_argument);
}

TEST_CASE("appkit view_for: the returned view works with the typed adapter API") {
    ensure_nsapp();
    @autoreleasepool {
        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 80, 22)];
        AppKitAdapter adapter;

        auto& v = adapter.view_for(field);
        adapter.set_text(v, "alice");
        CHECK(adapter.get_text(v) == "alice");
        CHECK([field.stringValue isEqualToString:@"alice"]);
    }
}

TEST_CASE("appkit view_for: binding through a cached view stays live") {
    ensure_nsapp();
    @autoreleasepool {
        auto adapter = std::make_shared<AppKitAdapter>();
        binding::BindingEngine engine(adapter);

        NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 120, 22)];
        Property<std::string> name("Alice");

        engine.bind_text_oneway(name, adapter->view_for(label));
        CHECK([label.stringValue isEqualToString:@"Alice"]);

        name = "Bob";
        CHECK([label.stringValue isEqualToString:@"Bob"]);
    }
}

TEST_CASE("appkit view_for: a Computed source binds one-way") {
    ensure_nsapp();
    @autoreleasepool {
        auto adapter = std::make_shared<AppKitAdapter>();
        binding::BindingEngine engine(adapter);

        Property<std::string> first("Ada");
        Property<std::string> last("Lovelace");
        Computed<std::string> full([&] { return first.get() + " " + last.get(); });

        NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 200, 22)];
        engine.bind_text_oneway(full, adapter->view_for(label));

        CHECK([label.stringValue isEqualToString:@"Ada Lovelace"]);
        last = "Byron";
        CHECK([label.stringValue isEqualToString:@"Ada Byron"]);
    }
}

TEST_CASE("appkit release_view: releases the bindings for one control") {
    ensure_nsapp();
    @autoreleasepool {
        auto adapter = std::make_shared<AppKitAdapter>();
        binding::BindingEngine engine(adapter);

        NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 120, 22)];
        Property<std::string> name("Alice");
        engine.bind_text_oneway(name, adapter->view_for(label));
        CHECK([label.stringValue isEqualToString:@"Alice"]);

        // Host discards this control while keeping the adapter: the cached
        // view is destroyed, its destroy signal fires, and the engine drops
        // the binding.
        adapter->release_view(label);

        name = "Bob";
        CHECK([label.stringValue isEqualToString:@"Alice"]);   // no longer driven

        // A fresh view_for on the same control gives a NEW wrapper, and
        // binding through it works again.
        engine.bind_text_oneway(name, adapter->view_for(label));
        CHECK([label.stringValue isEqualToString:@"Bob"]);
    }
}

TEST_CASE("appkit release_view: unknown / nil handles are harmless no-ops") {
    ensure_nsapp();
    @autoreleasepool {
        AppKitAdapter adapter;
        NSTextField* never_cached =
            [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 80, 22)];
        adapter.release_view(never_cached);   // never went through view_for
        adapter.release_view(nil);
        CHECK(true);                          // reaching here is the assertion
    }
}

TEST_CASE("appkit view_for: adapter teardown with an active signal bridge "
          "does not deadlock") {
    ensure_nsapp();
    @autoreleasepool {
        // Regression guard for the `~Impl` locking order. Destroying a cached
        // AppKitView fires on_destroy, and one of those handlers is the
        // adapter's own `bridges.erase(...)` lambda, which re-locks the
        // adapter mutex. If ~Impl destroyed the view cache while holding
        // that mutex, this test would hang instead of finishing.
        //
        // `on_text_changed` is what installs a bridge + the destroy
        // subscription, so it must be exercised for the guard to be real.
        NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 80, 22)];
        int hits = 0;
        {
            AppKitAdapter adapter;
            auto& v = adapter.view_for(field);
            auto sub = adapter.on_text_changed(v, [&hits](std::string_view) { ++hits; });
            adapter.set_text(v, "x");
            CHECK(adapter.get_text(v) == "x");
            // ~adapter runs here: view cache teardown → on_destroy →
            // bridges.erase → relock. Must complete.
        }
        CHECK(true);
    }
}

TEST_CASE("appkit view_for: engine outliving the adapter releases cleanly") {
    ensure_nsapp();
    @autoreleasepool {
        NSTextField* label = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 120, 22)];
        Property<std::string> name("Alice");

        auto adapter = std::make_shared<AppKitAdapter>();
        binding::BindingEngine engine(adapter);
        engine.bind_text_oneway(name, adapter->view_for(label));
        CHECK([label.stringValue isEqualToString:@"Alice"]);

        // Drop the host's handle; the engine still holds a shared_ptr, so the
        // adapter survives. Then clear the engine and write again.
        adapter.reset();
        engine.clear();
        name = "Bob";
        CHECK([label.stringValue isEqualToString:@"Alice"]);
    }
}
