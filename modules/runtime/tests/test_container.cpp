#include <doctest/doctest.h>

#include "aria/runtime/container.hpp"
#include <memory>
#include <string>

using namespace aria::runtime;

namespace {

struct ILogger {
    virtual ~ILogger() = default;
    virtual std::string log(const std::string&) = 0;
};

struct ConsoleLogger : ILogger {
    std::string log(const std::string& m) override { return "[console] " + m; }
};

struct FileLogger : ILogger {
    std::string log(const std::string& m) override { return "[file] " + m; }
};

}  // namespace

TEST_CASE("Container: singleton resolves same instance") {
    Container c;
    c.register_singleton<ILogger, ConsoleLogger>();
    auto a = c.resolve<ILogger>();
    auto b = c.resolve<ILogger>();
    CHECK(a.get() == b.get());
    CHECK(a->log("hi") == "[console] hi");
}

TEST_CASE("Container: register_instance uses provided shared_ptr") {
    Container c;
    auto provided = std::make_shared<FileLogger>();
    c.register_instance<ILogger>(provided);
    auto resolved = c.resolve<ILogger>();
    CHECK(resolved.get() == provided.get());
    CHECK(resolved->log("x") == "[file] x");
}

TEST_CASE("Container: factory yields fresh instance each time") {
    Container c;
    int call_count = 0;
    c.register_factory<ILogger>([&]() {
        ++call_count;
        return std::make_shared<ConsoleLogger>();
    });
    auto a = c.resolve<ILogger>();
    auto b = c.resolve<ILogger>();
    CHECK(call_count == 2);
    CHECK(a.get() != b.get());
}

TEST_CASE("Container: unregistered type throws") {
    Container c;
    CHECK_THROWS_AS((void)c.resolve<ILogger>(), std::runtime_error);
}

TEST_CASE("Container: has() reports registration state") {
    Container c;
    CHECK_FALSE(c.has<ILogger>());
    c.register_singleton<ILogger, ConsoleLogger>();
    CHECK(c.has<ILogger>());
    c.clear();
    CHECK_FALSE(c.has<ILogger>());
}
