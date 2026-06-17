// Minimal aria app — copy this into your own project.

#include "aria/aria.hpp"
#include <iostream>

using namespace aria;

int main() {
    Property<std::string> name{"World"};
    Computed<std::string> greeting{
        [&]{ return "Hello, " + name.get() + "!"; },
        name
    };

    auto sub = greeting.bind([](const std::string& s) {
        std::cout << s << "\n";
    });

    name = "Alice";
    name = "Bob";
    return 0;
}
