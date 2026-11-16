#include <aria/i18n/i18n.hpp>
#include <aria/property.hpp>

class Catalog final : public aria::i18n::Catalog {
public:
    aria::i18n::LookupResult lookup_exact(std::string_view locale, std::string_view,
                                         std::string_view key) const override {
        if (key != "title") return std::optional<std::string>{};
        return std::optional<std::string>{locale == "en" ? "Title" : "标题"};
    }
    std::vector<std::string> available_languages() const override { return {"en", "zh-CN"}; }
    std::expected<void, aria::i18n::CatalogError> reload() override { return {}; }
};

int main() {
    auto context = std::make_shared<aria::i18n::LocaleContext>(std::make_shared<Catalog>(),
        aria::i18n::LocaleOptions{.language = "en", .default_language = "en"});
    aria::Property<std::string> title;
    aria::i18n::TextBindings bindings{context, "sample"};
    bindings.text(title, "title");
    if (title.get() != "Title") return 1;
    context->set_language("zh-CN");
    return title.get() == "标题" ? 0 : 2;
}
