#pragma once
//
// ThemeVm — Tab 8: theme switcher (DI Container)
// Three ITheme implementations (Light / Dark / Solarized). Switching
// the ComboBox re-registers the binding in the Container, and the next
// resolve<ITheme>() returns the currently selected implementation.
//

#include "aria/aria.hpp"
#include "aria/runtime/container.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace showcase::theme {

struct ITheme {
    virtual ~ITheme() = default;
    virtual std::string_view name()       const noexcept = 0;
    virtual std::string_view cardBg()     const noexcept = 0;
    virtual std::string_view cardFg()     const noexcept = 0;
    virtual std::string_view cardBorder() const noexcept = 0;
};

class ThemeVm {
public:
    ThemeVm();

    aria::Property<std::string> currentName{"Light"};

    void pick(const std::string& name);
    [[nodiscard]] std::shared_ptr<ITheme> theme() const;

private:
    std::shared_ptr<aria::runtime::Container> container_;
};

}  // namespace showcase::theme
