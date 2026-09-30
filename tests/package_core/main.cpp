#include <aria/aria.hpp>

#if defined(_MSVC_LANG)
static_assert(_MSVC_LANG > 202002L, "aria::core must propagate C++23 to consumers");
#else
static_assert(__cplusplus > 202002L, "aria::core must propagate C++23 to consumers");
#endif

int main() {
    aria::Property<int> source{21};
    aria::Computed<int> doubled{[&] { return source.get() * 2; }};
    if (doubled.get() != 42) return 1;
    source.set(25);
    return doubled.get() == 50 ? 0 : 2;
}
