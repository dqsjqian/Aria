# Localization and application composition

Two optional modules extend Aria without adding UI, networking or JSON dependencies:

| Build option | Installed target | Public include |
|---|---|---|
| `ARIA_BUILD_I18N=ON` | `aria::i18n` | `<aria/i18n/i18n.hpp>` |
| `ARIA_BUILD_APP=ON` | `aria::app` | `<aria/app/app.hpp>` |

They are independent. The `aria::aria` aggregate continues to contain the base
layers only. Enable the options when building the SDK, then request the required
components in the application:

```cmake
find_package(aria CONFIG REQUIRED COMPONENTS i18n app)
target_link_libraries(my_app PRIVATE aria::i18n aria::app)
```

## Explicit locale ownership

Create a `LocaleContext` for an application or a window and pass its shared
pointer to ViewModels. There is no process-global translation backend.

```cpp
auto catalog = std::make_shared<aria::i18n::XmlCatalog>(resource_directory, "zh-CN");
auto locale = std::make_shared<aria::i18n::LocaleContext>(catalog,
    aria::i18n::LocaleOptions{
        .language = "en", .default_language = "zh-CN", .common_domain = "common"});
aria::Property<std::string> title;
aria::i18n::TextBindings texts{locale, "notes"};
texts.text(title, "title");
locale->set_language("zh-CN");
```

`TextBindings` owns subscriptions. Destroy it or call `clear()` to detach them.
It can be composed with an existing `binding::ViewModel`; no subclass or change
to the ViewModel ABI is required. `localize(fn)` tracks reactive values read by
the callback, including the language, catalog revision and formatting arguments.
Keep callback captures alive for the binding lifetime. A `text()` binding also
checks its target property's node lifetime before writing.

The default `DomainFirst` lookup order preserves the AriaTools resource policy:
requested domain/current language, requested domain/default language, common
domain/current language, common domain/default language. `LanguageFirst` checks
the common domain in the current language before falling back to the default
language. Empty strings count as present translations. `lookup_exact()` does
no fallback; `find()` returns `expected<optional<string>, CatalogError>`, and
`tr()` returns a `[domain/key]` marker for a missing key or throws on damaged
resources. An empty domain skips the domain-specific candidates and uses the
configured common domain; with no common domain configured it is a miss.

`XmlCatalog` reads `root/domain/strings.xml` and
`root/domain/strings_<locale>.xml`. Its documented XML subset supports UTF-8,
entities (including supplementary Unicode code points), comments and CDATA.
Duplicate keys, malformed XML, DTDs, nested markup and unsupported attributes
are reported as errors. It is not an Android resource compiler or a general XML
parser. Pluralization, ICU formatting, operating-system locale detection and
packaged-resource extraction remain application/backend responsibilities.

Resources load lazily. `reload()` validates all tables previously requested by
that catalog before replacing cached data; on failure it preserves the cache
and the context revision. Domains not yet requested remain lazy. Available
languages are a union of resource filenames, not a guarantee of completeness.
Custom resource formats implement `Catalog` with the same exact-lookup contract.

Use locale contexts and their reactive bindings on the reactive graph's owner
thread. `XmlCatalog` synchronizes its own cache, which does not make a
`LocaleContext` safe to read from a worker. Capture a language string before
dispatching work, or return to the UI executor before translating UI state.

## Module and ViewModel ownership

`ModuleRegistry<T>` stores an application-defined module object with a
`ModuleDescriptor { id, order }`. `ordered()` is stable for equal priorities;
duplicate IDs and null module objects are rejected. A module is not required
to own a page or inherit a framework interface. Applications keep business
metadata such as menu labels, permissions and platform view builders.

`ViewModelProvider<Context>` takes the application's context by reference:

```cpp
aria::app::ViewModelProvider<AppContext> views{context};
aria::SubscriptionBag registrations;
registrations += views.add("notes", [](AppContext& ctx) {
    return std::make_shared<NotesVm>(ctx.locale);
});
auto primary = views.get("notes");       // lazy, cached instance
auto page = views.create("notes");       // fresh instance
auto typed = views.get_as<NotesVm>("notes"); // checked dynamic cast
```

The context must outlive the provider. A null factory result is not cached;
exceptions also permit a later retry. Unknown IDs return null. Recursive
construction of the same ID throws `logic_error`. Removing a registration
invalidates that cached instance; unrelated registrations preserve other
cached instances. Existing external shared pointers continue to own their VM.
During provider destruction, `get()` and `create()` return null and `add()`
rejects new factories. Cached VM destructors can safely query the closing
provider without rebuilding its cache.

`ModuleLifecycle::start(steps)` stages cleanup handles. If a step throws, all
earlier steps are released in reverse order; a later start can retry. A step
must use its own RAII guards for partial work before returning its handle.
`stop()` and destruction release successful steps in reverse order. It does
not undo arbitrary external effects or load shared-library plugins.

## Contributions, slots and typed navigation targets

`ExtensionRegistry<Key, Value>` provides a single enabled contribution per key,
duplicate detection, enable/disable, snapshots and change subscriptions.
`add()` returns an `aria::Subscription`; **store the handle** for as long as
the contribution should be registered. Explicit removal followed by re-use of
the same key is safe: releasing the old handle cannot remove the replacement.
`snapshot()` preserves registration order. Callback failures follow Aria's
existing callback-failure reporting channel.

`MountRegistry<Context>` builds on this mechanism. `provide(slot, provider_id,
factory)` registers a VM factory; `resolve(slot)` returns its provider ID and
VM. `resolve<Interface>(slot, payload)` requires a successful dynamic cast and
`Interface::on_mount(payload)` returning true. The payload type is unrestricted
and does not impose a JSON dependency. Missing/disabled slots, null factory results,
wrong interfaces and rejected payloads produce no mount. Factory exceptions
propagate to the caller.

Resolution snapshots the selected registration before calling user code. A
factory may remove or replace itself; the current resolution still uses that
snapshot, and subsequent resolutions see the new state. Change notifications
allow hosts to refresh when another module toggles or removes a contribution.

`TypedRouteRegistry<Context>` similarly resolves factories by interface type
and calls `Interface::on_navigate(payload)`. The application then pushes the
resolved VM through the existing `binding::Navigator`, optionally wrapping it
with business or platform presentation metadata. Module IDs, slot names,
window presentation and concrete view construction remain outside this module.

All registries, registration handles and lifecycle operations belong to one
application thread. They do not provide thread-safe dynamic plugin unloading.

## Migrating existing applications

1. Introduce an explicit shared locale context in the application service
   assembly. Keep resource paths, default languages and fallback order stable.
2. Replace copied XML/translation engines with `XmlCatalog` and `LocaleContext`.
   Replace VM-specific language subscriptions with composed `TextBindings`.
3. Replace local registry/cache containers with the application module. Keep
   thin bridges where the application owns JSON payloads or page presentation.
4. Hold registration handles at the module/application scope, use rollback
   guards during startup, and destroy contributions before their services.
5. Validate real resource bundles and installed SDK consumption, then test
   locale isolation, VM destruction, startup failure/retry and slot removal.

Consumers need an SDK actually built with these options. A lockfile pointing
to an older released Aria does not gain new components automatically. During
development use an explicit local SDK prefix or source override and compatible
ForgeRecipes tooling; update release locks only when the corresponding
framework and recipe releases exist.
