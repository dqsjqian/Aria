#include <aria/app/app.hpp>

struct Context {};
int main() {
    Context context;
    aria::app::ViewModelProvider<Context> provider{context};
    auto registration = provider.add("sample", [](Context&) {
        return std::make_shared<aria::binding::ViewModel>();
    });
    auto primary = provider.get("sample");
    if (!primary || provider.get("sample") != primary || provider.create("sample") == primary) return 1;
    registration.release();
    return provider.get("sample") ? 2 : 0;
}
