#include <aria/binding/testing/fake_adapter.hpp>

int main() {
    aria::binding::testing::FakeAdapter adapter;
    aria::binding::testing::FakeView view;
    int changed = 0;
    auto subscription = adapter.on_text_changed(view, [&](std::string_view text) {
        if (text == "installed SDK") ++changed;
    });
    adapter.set_text(view, "installed SDK");
    adapter.set_text(view, "installed SDK");
    return changed == 1 && adapter.get_text(view) == "installed SDK" ? 0 : 1;
}
