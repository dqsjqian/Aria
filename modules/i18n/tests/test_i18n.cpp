#include <doctest/doctest.h>

#include "aria/i18n/i18n.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace {
using namespace aria::i18n;
namespace fs = std::filesystem;

class MemoryCatalog final : public Catalog {
public:
    using Identity = std::tuple<std::string, std::string, std::string>;
    std::map<Identity, std::string> values;
    LookupResult lookup_exact(std::string_view locale, std::string_view domain,
                              std::string_view key) const override {
        const auto found = values.find(Identity{locale, domain, key});
        if (found == values.end()) return std::optional<std::string>{};
        return std::optional<std::string>{found->second};
    }
    std::vector<std::string> available_languages() const override { return {"en", "zh-CN"}; }
    std::expected<void, CatalogError> reload() override { return {}; }
};

class TempResources {
public:
    TempResources() {
        static std::atomic<unsigned> serial{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("aria-i18n-" + std::to_string(stamp) + "-" +
                                          std::to_string(serial.fetch_add(1)));
        fs::create_directories(root);
    }
    ~TempResources() { std::error_code error; fs::remove_all(root, error); }
    void write(std::string_view relative, std::string_view content) const {
        const auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << content;
        if (!output) throw std::runtime_error("cannot write test resource");
    }
    fs::path root;
};

std::string document(std::string_view value) {
    return "<resources><string name=\"title\">" + std::string(value) + "</string></resources>";
}

std::shared_ptr<LocaleContext> context(std::shared_ptr<Catalog> catalog,
                                     FallbackOrder order = FallbackOrder::DomainFirst) {
    return std::make_shared<LocaleContext>(std::move(catalog), LocaleOptions{
        .language = "en", .default_language = "zh-CN", .common_domain = "common", .fallback_order = order});
}
}  // namespace

TEST_CASE("i18n exact lookup does not fall back and policies have explicit precedence") {
    auto catalog = std::make_shared<MemoryCatalog>();
    catalog->values[{"zh-CN", "page", "title"}] = "module default";
    catalog->values[{"en", "common", "title"}] = "common current";
    catalog->values[{"zh-CN", "common", "shared"}] = "shared default";
    auto domain_first = context(catalog);
    auto exact = domain_first->lookup_exact("en", "page", "title");
    REQUIRE(exact);
    CHECK_FALSE(exact->has_value());
    CHECK(domain_first->tr("page", "title") == "module default");
    CHECK(context(catalog, FallbackOrder::LanguageFirst)->tr("page", "title") == "common current");
    CHECK(domain_first->tr("page", "shared") == "shared default");
    CHECK(domain_first->tr("page", "missing") == "[page/missing]");
    CHECK(domain_first->tr("", "title") == "common current");
    CHECK(domain_first->tr("page", "title", "zh-CN") == "module default");
    CHECK(domain_first->language() == "en");
}

TEST_CASE("i18n contexts isolate locale and share backend without a global singleton") {
    auto catalog = std::make_shared<MemoryCatalog>();
    catalog->values[{"en", "page", "title"}] = "English";
    catalog->values[{"zh-CN", "page", "title"}] = "Chinese";
    auto first = context(catalog);
    auto second = context(catalog);
    aria::Property<std::string> first_text, second_text;
    auto first_binding = bind_text(first, "page", first_text, "title");
    auto second_binding = bind_text(second, "page", second_text, "title");
    int notifications = 0;
    auto subscription = first->on_language_changed([&](const auto&) { ++notifications; });
    first->set_language("zh-CN");
    CHECK(first_text.get() == "Chinese");
    CHECK(second_text.get() == "English");
    CHECK(second->language() == "en");
    first->set_language("zh-CN");
    CHECK(notifications == 1);
    CHECK_THROWS_AS(first->set_language(""), std::invalid_argument);
}

TEST_CASE("i18n binding owns context and releases subscriptions deterministically") {
    auto catalog = std::make_shared<MemoryCatalog>();
    catalog->values[{"en", "page", "title"}] = "before";
    auto selected = context(catalog);
    std::weak_ptr<LocaleContext> weak = selected;
    aria::Property<std::string> text;
    auto binding = bind_text(selected, "page", text, "title");
    selected.reset();
    CHECK_FALSE(weak.expired());
    binding.release();
    CHECK(weak.expired());
    CHECK(text.get() == "before");
}

TEST_CASE("i18n text binding tolerates target destruction before subscription") {
    auto selected = context(std::make_shared<MemoryCatalog>());
    auto target = std::make_unique<aria::Property<std::string>>();
    auto binding = bind_text(selected, "page", *target, "title");
    target.reset();
    CHECK_NOTHROW(selected->set_language("zh-CN"));
    CHECK_NOTHROW((void)selected->reload());
    binding.release();
}

TEST_CASE("i18n composition tracks dynamic arguments and clears independently of VM inheritance") {
    auto catalog = std::make_shared<MemoryCatalog>();
    catalog->values[{"en", "page", "items"}] = "items";
    catalog->values[{"zh-CN", "page", "items"}] = "entries";
    auto selected = context(catalog);
    aria::Property<int> count{1};
    aria::Property<std::string> output;
    int runs = 0;
    TextBindings bindings(selected, "page");
    bindings.localize([&] {
        ++runs;
        output.set(std::to_string(count.get()) + " " + selected->tr("page", "items"));
    });
    CHECK(output.get() == "1 items");
    count.set(2);
    CHECK(output.get() == "2 items");
    selected->set_language("zh-CN");
    CHECK(output.get() == "2 entries");
    CHECK(runs == 3);
    bindings.clear();
    count.set(3);
    selected->set_language("en");
    CHECK(runs == 3);
    CHECK(output.get() == "2 entries");
}

TEST_CASE("i18n Computed translations track language and successful catalog reload") {
    auto catalog = std::make_shared<MemoryCatalog>();
    catalog->values[{"en", "page", "title"}] = "old";
    auto selected = context(catalog);
    aria::Computed<std::string> translated{[&] { return selected->tr("page", "title"); }};
    CHECK(translated.get() == "old");
    catalog->values[{"en", "page", "title"}] = "new";
    CHECK(translated.get() == "old");
    REQUIRE(selected->reload());
    CHECK(translated.get() == "new");
    CHECK(selected->revision() == 1);
}

TEST_CASE("i18n XML loads UTF-8 entities comments CDATA empty values and normalizes newlines") {
    TempResources resources;
    resources.write("page/strings.xml",
        "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!-- <string name='fake'>not a resource</string> -->\n<resources>"
        "<string name='title'>中文 &#x1F600; &#128512; &amp;&lt;&gt;&quot;&apos;"
        "<![CDATA[<raw>&literal;]]><!-- omitted -->\r\nnext\rline</string>"
        "<string name='empty'/></resources>");
    XmlCatalog catalog(resources.root.string(), "zh-CN");
    auto result = catalog.lookup_exact("zh-CN", "page", "title");
    REQUIRE(result);
    REQUIRE(result->has_value());
    CHECK(**result == "中文 😀 😀 &<>\"'<raw>&literal;\nnext\nline");
    auto empty = catalog.lookup_exact("zh-CN", "page", "empty");
    REQUIRE(empty);
    REQUIRE(empty->has_value());
    CHECK(**empty == "");
    auto fake = catalog.lookup_exact("zh-CN", "page", "fake");
    REQUIRE(fake);
    CHECK_FALSE(fake->has_value());
    auto absent = catalog.lookup_exact("en", "page", "title");
    REQUIRE(absent);
    CHECK_FALSE(absent->has_value());
}

TEST_CASE("i18n XML rejects damaged resources with typed diagnostics instead of treating errors as misses") {
    const std::vector<std::string> invalid = {
        "<resources><string name='a'>one</string><string name='a'>two</string></resources>",
        "<resources><string name='a'>unterminated</resources>",
        "<resources><string name='a' name='b'>duplicate attribute</string></resources>",
        "<resources><string name='a'><b>nested</b></string></resources>",
        "<resources><string name='a' translatable='false'>unsupported attribute</string></resources>",
        "<resources><string name='a'>&unknown;</string></resources>",
        "<resources><string name='a'>&#xD800;</string></resources>",
        "<resources><string name='a'>&#x110000;</string></resources>",
        "<resources><string name='a'>&#-1;</string></resources>",
        "<resources><string name='a'>&#12junk;</string></resources>",
        "<resources><string name='a'>&#0;</string></resources>",
        "<resources><!-- invalid -- comment --><string name='a'>x</string></resources>",
        "<resources><string name='a'>]]></string></resources>",
        "<!DOCTYPE resources [<!ENTITY x 'bad'>]><resources/>",
        "<?xml version='1.0' encoding='UTF-16'?><resources/>",
        "<resources/>trailing",
        std::string("<resources><string name='a'>") + "\xF0\x28\x8C\x28" + "</string></resources>",
        std::string("<resources><string name='a'>") + "\xC0\xAF" + "</string></resources>",
        std::string("<resources><string name='a'>") + '\0' + "</string></resources>"
    };
    TempResources resources;
    for (const auto& xml : invalid) {
        CAPTURE(xml);
        resources.write("page/strings.xml", xml);
        auto catalog = std::make_shared<XmlCatalog>(resources.root.string(), "zh-CN");
        auto result = catalog->lookup_exact("zh-CN", "page", "a");
        REQUIRE_FALSE(result);
        CHECK(result.error().code == CatalogErrorCode::parse);
        CHECK(fs::path(result.error().path) == resources.root / "page" / "strings.xml");
        CHECK_FALSE(result.error().message.empty());
        CHECK_THROWS_AS((void)context(catalog)->tr("page", "a", "zh-CN"), std::runtime_error);
    }
}

TEST_CASE("i18n XML missing resources fall back but corrupt primary language does not") {
    TempResources resources;
    resources.write("page/strings.xml", document("default"));
    auto selected = context(std::make_shared<XmlCatalog>(resources.root.string(), "zh-CN"));
    CHECK(selected->tr("page", "title") == "default");
    resources.write("page/strings_en.xml", "<resources><string");
    const auto failed = selected->reload();
    REQUIRE_FALSE(failed);
    CHECK(selected->revision() == 0);
    CHECK(selected->tr("page", "title") == "default");
    auto fresh = context(std::make_shared<XmlCatalog>(resources.root.string(), "zh-CN"));
    CHECK_FALSE(fresh->find("page", "title"));
}

TEST_CASE("i18n XML reload is transactional updates bindings and revives cached missing resources") {
    TempResources resources;
    resources.write("page/strings.xml", document("default"));
    resources.write("page/strings_en.xml", document("before"));
    auto catalog = std::make_shared<XmlCatalog>(resources.root.string(), "zh-CN");
    auto selected = context(catalog);
    aria::Property<std::string> text;
    auto binding = bind_text(selected, "page", text, "title");
    auto missing = selected->lookup_exact("fr", "page", "title");
    REQUIRE(missing);
    CHECK_FALSE(missing->has_value());
    int changes = 0;
    auto changed = selected->on_catalog_changed([&](const auto&) { ++changes; });
    resources.write("page/strings_en.xml", document("after"));
    resources.write("page/strings_fr.xml", document("français"));
    REQUIRE(selected->reload());
    CHECK(text.get() == "after");
    CHECK(selected->tr("page", "title", "fr") == "français");
    CHECK(changes == 1);
    resources.write("page/strings_en.xml", document("uncommitted"));
    resources.write("page/strings_fr.xml", "broken");
    CHECK_FALSE(selected->reload());
    CHECK(text.get() == "after");
    CHECK(selected->tr("page", "title", "fr") == "français");
    CHECK(changes == 1);
    fs::remove(resources.root / "page/strings_fr.xml");
    REQUIRE(selected->reload());
    CHECK(text.get() == "uncommitted");
    CHECK(selected->tr("page", "title", "fr") == "default");
}

TEST_CASE("i18n XML validates namespace paths and discovers open sorted language identifiers") {
    TempResources resources;
    resources.write("page/strings_en-US.xml", document("US"));
    resources.write("other/strings_fr.xml", document("FR"));
    XmlCatalog catalog(resources.root.string(), "zh-CN");
    CHECK(catalog.available_languages() == std::vector<std::string>{"en-US", "fr", "zh-CN"});
    for (const auto* invalid : {"../outside", "/absolute", "..", "a\\b", ""}) {
        CHECK_FALSE(catalog.lookup_exact("en", invalid, "title"));
        CHECK_FALSE(catalog.lookup_exact(invalid, "page", "title"));
    }
    const auto selected = catalog.lookup_exact("en-US", "page", "title");
    REQUIRE(selected);
    REQUIRE(selected->has_value());
    CHECK(**selected == "US");
    auto exact = catalog.lookup_exact("en", "page", "title");
    REQUIRE(exact);
    CHECK_FALSE(exact->has_value());  // Hosts, not filenames, choose locale aliases.
}

TEST_CASE("i18n XML exact backend reads can be concurrent without reading reactive locale") {
    TempResources resources;
    resources.write("page/strings.xml", document("stable"));
    auto catalog = std::make_shared<XmlCatalog>(resources.root.string(), "zh-CN");
    std::atomic<bool> valid{true};
    std::vector<std::jthread> readers;
    for (int worker = 0; worker < 4; ++worker) {
        readers.emplace_back([&] {
            for (int iteration = 0; iteration < 50; ++iteration) {
                const auto value = catalog->lookup_exact("zh-CN", "page", "title");
                if (!value || !*value || **value != "stable") valid = false;
            }
        });
    }
    readers.clear();
    CHECK(valid.load());
}
