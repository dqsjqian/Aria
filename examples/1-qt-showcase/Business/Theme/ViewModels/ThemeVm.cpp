#include "ThemeVm.h"

namespace showcase::theme {

namespace {

struct LightTheme : ITheme {
    std::string_view name()       const noexcept override { return "Light"; }
    std::string_view cardBg()     const noexcept override { return "#ffffff"; }
    std::string_view cardFg()     const noexcept override { return "#212121"; }
    std::string_view cardBorder() const noexcept override { return "#e0e0e0"; }
};

struct DarkTheme : ITheme {
    std::string_view name()       const noexcept override { return "Dark"; }
    std::string_view cardBg()     const noexcept override { return "#263238"; }
    std::string_view cardFg()     const noexcept override { return "#eceff1"; }
    std::string_view cardBorder() const noexcept override { return "#455a64"; }
};

struct SolarizedTheme : ITheme {
    std::string_view name()       const noexcept override { return "Solarized"; }
    std::string_view cardBg()     const noexcept override { return "#fdf6e3"; }
    std::string_view cardFg()     const noexcept override { return "#586e75"; }
    std::string_view cardBorder() const noexcept override { return "#eee8d5"; }
};

}  // namespace

ThemeVm::ThemeVm() : container_(std::make_shared<aria::runtime::Container>()) {
    container_->register_singleton<ITheme, LightTheme>();
}

void ThemeVm::pick(const std::string& name) {
    container_->clear();
    if      (name == "Light")     container_->register_singleton<ITheme, LightTheme>();
    else if (name == "Dark")      container_->register_singleton<ITheme, DarkTheme>();
    else if (name == "Solarized") container_->register_singleton<ITheme, SolarizedTheme>();
    currentName.set(name);
}

std::shared_ptr<ITheme> ThemeVm::theme() const {
    return container_->resolve<ITheme>();
}

}  // namespace showcase::theme
