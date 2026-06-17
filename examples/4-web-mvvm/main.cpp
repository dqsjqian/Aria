/// @file main.cpp
/// @brief Example 4 — Web MVVM via HttpAdapter.
///
/// Spawns a tiny ViewModel (a counter + a greeting) and exposes it
/// to a browser at http://localhost:9090. Open the URL and watch the
/// page update reactively as the C++ ViewModel changes.

#include "aria/adapters/http/http_adapter.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    using namespace aria::adapters::http;

    HttpAdapterConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 19090;
    cfg.enable_cors = true;
    if (argc >= 2) {
        // Optional: serve static files from the given root.
        cfg.static_root = argv[1];
    }
    // Optional: enable HTTPS by passing cert + key as extra args.
    //   ./example_4_web_mvvm <static_root> <cert.pem> <key.pem>
    if (argc >= 4) {
        cfg.tls_cert_file = argv[2];
        cfg.tls_key_file  = argv[3];
        cfg.tls_min_version = "1.2";
    }

    HttpAdapter http(cfg);

    // Register views.
    auto& greeting = http.register_view("greeting", "text");
    auto& counter  = http.register_view("counter",  "int");
    auto& reset    = http.register_view("reset",    "click");

    // Initial values.
    http.set_text(greeting, "Hello from Aria!");
    http.set_int(counter, 0);

    // Subscribe to button click → reset counter.
    auto sub_click = http.on_click(reset, [&]() {
        http.set_int(counter, 0);
        std::cout << "[server] counter reset\n";
    });

    // Subscribe to text changes from browser.
    auto sub_text = http.on_text_changed(greeting,
        [&](std::string_view s) {
            std::cout << "[server] greeting changed to: " << s << "\n";
        });

    // Custom command: bump counter by N.
    http.register_command("counter", "bump",
        [&](std::string_view args_json) -> std::string {
            // Tiny inline parse: look for "by":<n>.
            int by = 1;
            auto pos = args_json.find("\"by\"");
            if (pos != std::string_view::npos) {
                pos = args_json.find(':', pos);
                if (pos != std::string_view::npos) {
                    try {
                        by = std::stoi(std::string(args_json.substr(pos + 1)));
                    } catch (...) {}
                }
            }
            int cur = http.get_int(counter);
            http.set_int(counter, cur + by);
            return std::string("{\"new_value\":") +
                   std::to_string(cur + by) + "}";
        });

    if (!http.start()) {
        std::cerr << "Failed to start server.\n";
        return 1;
    }
    const char* scheme = (argc >= 4) ? "https" : "http";
    std::cout << "Aria HTTP adapter listening on " << scheme << "://"
              << cfg.host << ":" << http.actual_port() << "\n";
    std::cout << "Open the page and try interacting with the views.\n";
    std::cout << "Press Ctrl-C to quit.\n";

    // Background timer ticking the counter every 5 seconds, just to
    // demonstrate server-driven state pushes.
    std::atomic<bool> stop{false};
    std::thread ticker([&]() {
        while (!stop) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            int cur = http.get_int(counter);
            http.set_int(counter, cur + 1);
        }
    });

    // Wait forever (Ctrl-C).
    std::this_thread::sleep_for(std::chrono::hours(24));

    stop = true;
    ticker.join();
    http.stop();
    return 0;
}
