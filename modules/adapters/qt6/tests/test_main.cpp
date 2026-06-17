#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <QApplication>

#include <cstdlib>
#include <vector>

int main(int argc, char* argv[]) {
    // Force the offscreen QPA platform so the test runs headlessly on CI.
    qputenv("QT_QPA_PLATFORM", "offscreen");

    // Build a synthetic argv that includes -platform offscreen.
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) args.push_back(argv[i]);
    static char arg_platform[] = "-platform";
    static char arg_offscreen[] = "offscreen";
    args.push_back(arg_platform);
    args.push_back(arg_offscreen);
    int new_argc = static_cast<int>(args.size());

    QApplication app(new_argc, args.data());

    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
