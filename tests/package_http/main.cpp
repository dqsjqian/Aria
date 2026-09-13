#include <aria/adapters/http/http_adapter.hpp>
#include <httplib.h>

#include <iostream>

#if defined(ARIA_HTTP_HAS_TLS) != defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#error The installed HTTP library and its public httplib header must agree on TLS
#endif

int main() {
    aria::adapters::http::HttpAdapterConfig config;
    config.port = 0;
    config.worker_threads = 2;
    aria::adapters::http::HttpAdapter adapter(config);
    adapter.native_server().Get("/package-consumer",
        [](const httplib::Request&, httplib::Response& response) {
            response.set_content("package-consumer", "text/plain");
        });

    // Exercise the installed header and exported dependencies without
    // opening a listener or issuing any network requests.
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient client("127.0.0.1", 443);
    if (!client.is_valid()) return 1;
    auto* context = SSL_CTX_new(TLS_method());
    if (!context) return 2;
    SSL_CTX_free(context);
    std::cout << "ARIA_HTTP_PACKAGE_PASS tls=1 "
              << OpenSSL_version(OPENSSL_VERSION) << '\n';
#else
    httplib::Client client("127.0.0.1", 80);
    if (!client.is_valid()) return 1;
    std::cout << "ARIA_HTTP_PACKAGE_PASS tls=0\n";
#endif
}
