// Exercise the HTTPS adapter with an independent OpenSSL client. Each test
// creates a short-lived key/certificate; no private key is stored in the repo.
#include "aria/adapters/http/http_adapter.hpp"
#include "test_http_client.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace {
using namespace aria::adapters::http;

struct CertificateFiles {
    std::filesystem::path directory;
    std::filesystem::path certificate;
    std::filesystem::path key;

    CertificateFiles() {
        std::array<unsigned char, 12> random{};
        REQUIRE(RAND_bytes(random.data(), static_cast<int>(random.size())) == 1);
        std::string name = "aria-https-test-";
        constexpr char hex[] = "0123456789abcdef";
        for (const auto byte : random) {
            name += hex[byte >> 4];
            name += hex[byte & 15];
        }
        directory = std::filesystem::temp_directory_path() / name;
        REQUIRE(std::filesystem::create_directory(directory));
        certificate = directory / "certificate.pem";
        key = directory / "key.pem";
    }

    ~CertificateFiles() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }

    void generate() const {
        using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
        KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        REQUIRE(context);
        REQUIRE(EVP_PKEY_keygen_init(context.get()) == 1);
        REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1);
        EVP_PKEY* raw_key = nullptr;
        REQUIRE(EVP_PKEY_keygen(context.get(), &raw_key) == 1);
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> private_key(raw_key, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        REQUIRE(cert);
        REQUIRE(X509_set_version(cert.get(), 2) == 1);
        REQUIRE(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) == 1);
        REQUIRE(X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60));
        REQUIRE(X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600));
        REQUIRE(X509_set_pubkey(cert.get(), private_key.get()) == 1);
        std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
        REQUIRE(subject);
        REQUIRE(X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
        REQUIRE(X509_set_subject_name(cert.get(), subject.get()) == 1);
        REQUIRE(X509_set_issuer_name(cert.get(), subject.get()) == 1);
        X509V3_CTX extension_context{};
        X509V3_set_ctx(&extension_context, cert.get(), cert.get(), nullptr, nullptr, 0);
        auto add_extension = [&](int nid, const char* value) {
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                X509V3_EXT_conf_nid(nullptr, &extension_context, nid, value), X509_EXTENSION_free);
            REQUIRE(extension);
            REQUIRE(X509_add_ext(cert.get(), extension.get(), -1) == 1);
        };
        add_extension(NID_basic_constraints, "critical,CA:TRUE");
        add_extension(NID_key_usage, "critical,digitalSignature,keyEncipherment,keyCertSign");
        add_extension(NID_ext_key_usage, "serverAuth");
        add_extension(NID_subject_alt_name, "DNS:localhost");
        REQUIRE(X509_sign(cert.get(), private_key.get(), EVP_sha256()) > 0);
        using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
        Bio cert_file(BIO_new_file(certificate.string().c_str(), "w"), BIO_free);
        REQUIRE(cert_file);
        REQUIRE(PEM_write_bio_X509(cert_file.get(), cert.get()) == 1);
        Bio key_file(BIO_new_file(key.string().c_str(), "w"), BIO_free);
        REQUIRE(key_file);
        REQUIRE(PEM_write_bio_PrivateKey(key_file.get(), private_key.get(), nullptr,
            nullptr, 0, nullptr, nullptr) == 1);
    }
};

HttpAdapterConfig tls_config(const CertificateFiles& files) {
    HttpAdapterConfig config;
    config.port = 0;
    config.worker_threads = 2;
    config.tls_cert_file = files.certificate.string();
    config.tls_key_file = files.key.string();
    return config;
}

nlohmann::json read_state(SSL* connection) {
    constexpr std::string_view request =
        "GET /aria/state?view=tls-state HTTP/1.1\r\n"
        "Host: localhost\r\nConnection: keep-alive\r\n\r\n";
    std::size_t sent = 0;
    REQUIRE(SSL_write_ex(connection, request.data(), request.size(), &sent) == 1);
    REQUIRE(sent == request.size());
    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < 65536) {
        const int received = SSL_read(connection, buffer.data(), static_cast<int>(buffer.size()));
        INFO("OpenSSL error: ", ERR_peek_last_error());
        REQUIRE(received > 0);
        response.append(buffer.data(), static_cast<std::size_t>(received));
        const auto separator = response.find("\r\n\r\n");
        if (separator == std::string::npos) continue;
        REQUIRE(response.starts_with("HTTP/1.1 200 "));
        auto body = nlohmann::json::parse(response.substr(separator + 4), nullptr, false);
        if (!body.is_discarded()) return body;
    }
    FAIL("HTTPS response exceeded the test's bounded response size");
    return {};
}
} // namespace

TEST_CASE("HTTPS returns state with independent certificate and hostname verification") {
    CertificateFiles files;
    files.generate();
    HttpAdapter http(tls_config(files));
    auto& view = http.register_view("tls-state", "text");
    http.set_text(view, "verified HTTPS state");
    REQUIRE(http.start());

    using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
    Context context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    REQUIRE(context);
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
    REQUIRE(SSL_CTX_load_verify_locations(context.get(), files.certificate.string().c_str(), nullptr) == 1);
    test_http::Client socket(http.actual_port());
    REQUIRE(socket.connected());
    std::unique_ptr<SSL, decltype(&SSL_free)> connection(SSL_new(context.get()), SSL_free);
    REQUIRE(connection);
    REQUIRE(SSL_set_fd(connection.get(), static_cast<int>(socket.native_handle())) == 1);
    REQUIRE(SSL_set_tlsext_host_name(connection.get(), "localhost") == 1);

    SUBCASE("trusted matching hostname completes HTTPS and stop cancels pending TLS reads") {
        REQUIRE(X509_VERIFY_PARAM_set1_host(SSL_get0_param(connection.get()), "localhost", 0) == 1);
        REQUIRE(SSL_connect(connection.get()) == 1);
        CHECK(SSL_get_verify_result(connection.get()) == X509_V_OK);
        const auto state = read_state(connection.get());
        CHECK(state.at("value") == "verified HTTPS state");

        // Keep both an established idle TLS session and an unnegotiated TCP
        // connection alive while stop cancels their read/handshake operations.
        test_http::Client pending_handshake(http.actual_port());
        REQUIRE(pending_handshake.connected());
        const auto started = std::chrono::steady_clock::now();
        http.stop();
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        CHECK_FALSE(http.running());
        CHECK(http.actual_port() == 0);
    }
    SUBCASE("a trusted certificate does not bypass hostname verification") {
        REQUIRE(X509_VERIFY_PARAM_set1_host(SSL_get0_param(connection.get()), "wrong-host.invalid", 0) == 1);
        CHECK(SSL_connect(connection.get()) != 1);
        CHECK(SSL_get_verify_result(connection.get()) == X509_V_ERR_HOSTNAME_MISMATCH);
    }
}
