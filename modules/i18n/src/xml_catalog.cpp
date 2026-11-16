#include "aria/i18n/xml_catalog.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace aria::i18n {
namespace {

using Table = std::map<std::string, std::string, std::less<>>;
using TableResult = std::expected<Table, CatalogError>;
namespace fs = std::filesystem;

bool identifier(std::string_view value) {
    if (value.empty() || value == "." || value == "..") return false;
    for (const char ch : value) {
        const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (!letter && !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-' && ch != '.') return false;
    }
    return true;
}

bool xml_char(std::uint32_t code) {
    return code == 9 || code == 10 || code == 13 ||
           (code >= 0x20 && code <= 0xD7FF) ||
           (code >= 0xE000 && code <= 0xFFFD) ||
           (code >= 0x10000 && code <= 0x10FFFF);
}

void append_utf8(std::string& output, std::uint32_t code) {
    if (code < 0x80) {
        output += static_cast<char>(code);
    } else if (code < 0x800) {
        output += static_cast<char>(0xC0U | (code >> 6));
        output += static_cast<char>(0x80U | (code & 0x3FU));
    } else if (code < 0x10000) {
        output += static_cast<char>(0xE0U | (code >> 12));
        output += static_cast<char>(0x80U | ((code >> 6) & 0x3FU));
        output += static_cast<char>(0x80U | (code & 0x3FU));
    } else {
        output += static_cast<char>(0xF0U | (code >> 18));
        output += static_cast<char>(0x80U | ((code >> 12) & 0x3FU));
        output += static_cast<char>(0x80U | ((code >> 6) & 0x3FU));
        output += static_cast<char>(0x80U | (code & 0x3FU));
    }
}

struct ParseFailure { std::size_t offset; std::string message; };

class Parser {
public:
    explicit Parser(std::string_view xml) : xml_(xml) {}

    Table parse() {
        validate_utf8();
        if (starts("\xEF\xBB\xBF")) pos_ += 3;
        if (starts("<?xml")) declaration();
        misc();
        const auto root = opening("resources");
        if (!root.attributes.empty()) fail("resources attributes are unsupported");
        Table table;
        if (!root.empty) {
            for (;;) {
                misc();
                if (starts("</resources")) { closing("resources"); break; }
                auto tag = opening("string");
                if (tag.attributes.size() != 1 || !tag.attributes.contains("name") ||
                    tag.attributes.at("name").empty()) {
                    fail("string requires exactly one nonempty name attribute");
                }
                auto key = std::move(tag.attributes.at("name"));
                std::string text;
                if (!tag.empty) text = string_content();
                if (!table.emplace(std::move(key), std::move(text)).second) {
                    fail("duplicate string key");
                }
            }
        }
        misc();
        if (pos_ != xml_.size()) fail("unexpected content after resources");
        return table;
    }

private:
    struct Tag { Table attributes; bool empty = false; };
    std::string_view xml_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(std::string message) const { throw ParseFailure{pos_, std::move(message)}; }
    [[nodiscard]] bool starts(std::string_view token) const { return xml_.substr(pos_).starts_with(token); }
    void expect(std::string_view token) {
        if (!starts(token)) fail("expected " + std::string(token));
        pos_ += token.size();
    }
    static bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; }
    void whitespace() { while (pos_ < xml_.size() && space(xml_[pos_])) ++pos_; }
    void comment() {
        expect("<!--");
        const auto end = xml_.find("-->", pos_);
        if (end == std::string_view::npos) fail("unterminated comment");
        if (xml_.substr(pos_, end - pos_).find("--") != std::string_view::npos ||
            (end > pos_ && xml_[end - 1] == '-')) fail("invalid XML comment");
        pos_ = end + 3;
    }
    void misc() {
        whitespace();
        while (starts("<!--")) { comment(); whitespace(); }
    }
    std::string name() {
        const auto begin = pos_;
        while (pos_ < xml_.size()) {
            const char ch = xml_[pos_];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == ':')) break;
            ++pos_;
        }
        if (begin == pos_) fail("expected XML name");
        return std::string(xml_.substr(begin, pos_ - begin));
    }
    std::string decoded(std::string_view raw, bool entities, bool attribute = false) const {
        std::string result;
        for (std::size_t index = 0; index < raw.size(); ++index) {
            char ch = raw[index];
            if (ch == '\r') {
                if (index + 1 < raw.size() && raw[index + 1] == '\n') ++index;
                ch = '\n';
            }
            if (attribute && (ch == '\n' || ch == '\t')) ch = ' ';
            if (!entities || ch != '&') { result += ch; continue; }
            const auto end = raw.find(';', index + 1);
            if (end == std::string_view::npos) fail("unterminated entity");
            auto entity = raw.substr(index + 1, end - index - 1);
            if (entity == "amp") result += '&';
            else if (entity == "lt") result += '<';
            else if (entity == "gt") result += '>';
            else if (entity == "quot") result += '"';
            else if (entity == "apos") result += '\'';
            else if (entity.starts_with('#')) {
                entity.remove_prefix(1);
                int base = 10;
                if (entity.starts_with('x')) { base = 16; entity.remove_prefix(1); }
                std::uint32_t code = 0;
                const auto parsed = std::from_chars(entity.data(), entity.data() + entity.size(), code, base);
                if (entity.empty() || parsed.ec != std::errc{} ||
                    parsed.ptr != entity.data() + entity.size() || !xml_char(code)) {
                    fail("invalid numeric character entity");
                }
                append_utf8(result, code);
            } else fail("unknown entity (DTDs are unsupported)");
            index = end;
        }
        return result;
    }
    Table attributes(std::string_view terminator, bool* empty = nullptr) {
        Table result;
        for (;;) {
            const auto before_space = pos_;
            whitespace();
            if (starts(terminator)) { pos_ += terminator.size(); return result; }
            if (empty && starts("/>")) { pos_ += 2; *empty = true; return result; }
            if (pos_ == before_space) fail("attributes must be separated by whitespace");
            auto key = name();
            whitespace();
            expect("=");
            whitespace();
            if (pos_ == xml_.size() || (xml_[pos_] != '\'' && xml_[pos_] != '"')) {
                fail("expected quoted attribute");
            }
            const char quote = xml_[pos_++];
            const auto end = xml_.find(quote, pos_);
            if (end == std::string_view::npos) fail("unterminated attribute");
            const auto raw = xml_.substr(pos_, end - pos_);
            if (raw.find('<') != std::string_view::npos) fail("unescaped < in attribute");
            auto value = decoded(raw, true, true);
            pos_ = end + 1;
            if (!result.emplace(std::move(key), std::move(value)).second) fail("duplicate attribute");
        }
    }
    Tag opening(std::string_view tag_name) {
        expect("<");
        if (name() != tag_name) fail("expected " + std::string(tag_name) + " element");
        Tag tag;
        tag.attributes = attributes(">", &tag.empty);
        return tag;
    }
    void closing(std::string_view tag_name) {
        expect("</");
        if (name() != tag_name) fail("mismatched closing element");
        whitespace();
        expect(">");
    }
    void declaration() {
        expect("<?xml");
        const auto attrs = attributes("?>");
        const auto version = attrs.find("version");
        if (version == attrs.end() || version->second != "1.0") fail("only XML 1.0 is supported");
        for (const auto& [key, value] : attrs) {
            if (key == "version") continue;
            if (key == "encoding" && (value == "UTF-8" || value == "utf-8")) continue;
            if (key == "standalone" && (value == "yes" || value == "no")) continue;
            fail("unsupported XML declaration");
        }
    }
    std::string string_content() {
        std::string result;
        for (;;) {
            if (starts("</string")) { closing("string"); return result; }
            if (starts("<!--")) { comment(); continue; }
            if (starts("<![CDATA[")) {
                pos_ += 9;
                const auto end = xml_.find("]]>", pos_);
                if (end == std::string_view::npos) fail("unterminated CDATA");
                result += decoded(xml_.substr(pos_, end - pos_), false);
                pos_ = end + 3;
                continue;
            }
            if (pos_ == xml_.size()) fail("unterminated string element");
            if (xml_[pos_] == '<') fail("nested elements and declarations are unsupported");
            const auto end = xml_.find('<', pos_);
            const auto raw = xml_.substr(pos_, end == std::string_view::npos ? end : end - pos_);
            if (raw.find("]]>") != std::string_view::npos) fail("unexpected CDATA terminator");
            result += decoded(raw, true);
            pos_ = end == std::string_view::npos ? xml_.size() : end;
        }
    }
    void validate_utf8() {
        for (std::size_t index = 0; index < xml_.size();) {
            pos_ = index;
            const auto first = static_cast<unsigned char>(xml_[index++]);
            std::uint32_t code = first;
            std::size_t continuation = 0;
            std::uint32_t minimum = 0;
            if (first >= 0xC2 && first <= 0xDF) { continuation = 1; code &= 0x1FU; minimum = 0x80; }
            else if (first >= 0xE0 && first <= 0xEF) { continuation = 2; code &= 0x0FU; minimum = 0x800; }
            else if (first >= 0xF0 && first <= 0xF4) { continuation = 3; code &= 0x07U; minimum = 0x10000; }
            else if (first >= 0x80) fail("invalid UTF-8 leading byte");
            if (continuation > xml_.size() - index) fail("truncated UTF-8 sequence");
            for (std::size_t count = 0; count < continuation; ++count) {
                const auto next = static_cast<unsigned char>(xml_[index++]);
                if ((next & 0xC0U) != 0x80U) fail("invalid UTF-8 continuation");
                code = (code << 6) | (next & 0x3FU);
            }
            if (code < minimum || !xml_char(code)) fail("invalid XML Unicode character");
        }
        pos_ = 0;
    }
};

TableResult read_table(const fs::path& path) {
    std::error_code error;
    const auto status = fs::status(path, error);
    if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) return Table{};
    if (error || !fs::is_regular_file(status)) {
        return std::unexpected(CatalogError{CatalogErrorCode::io,
            error ? error.message() : "resource is not a regular file", path.string()});
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(CatalogError{CatalogErrorCode::io, "cannot open resource", path.string()});
    const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return std::unexpected(CatalogError{CatalogErrorCode::io, "cannot read resource", path.string()});
    try {
        return Parser(content).parse();
    } catch (const ParseFailure& failure) {
        return std::unexpected(CatalogError{CatalogErrorCode::parse, failure.message, path.string(), failure.offset});
    }
}

}  // namespace

struct XmlCatalog::Impl {
    fs::path root;
    std::string default_language;
    using Identity = std::pair<std::string, std::string>;  // locale, domain
    mutable std::map<Identity, TableResult> tables;
    mutable std::mutex mutex;

    [[nodiscard]] fs::path path(const Identity& identity) const {
        const auto& [locale, domain] = identity;
        return root / domain / (locale == default_language ? "strings.xml" : "strings_" + locale + ".xml");
    }
};

XmlCatalog::XmlCatalog(std::string root, std::string default_language)
    : impl_(std::make_unique<Impl>()) {
    if (!identifier(default_language)) throw std::invalid_argument("invalid XML default language identifier");
    impl_->root = std::move(root);
    impl_->default_language = std::move(default_language);
}
XmlCatalog::~XmlCatalog() = default;

LookupResult XmlCatalog::lookup_exact(std::string_view locale, std::string_view domain,
                                      std::string_view key) const {
    if (!identifier(locale) || !identifier(domain)) {
        return std::unexpected(CatalogError{CatalogErrorCode::invalid_identifier,
            "locale/domain must be a nonempty filename identifier", impl_->root.string()});
    }
    const Impl::Identity identity{locale, domain};
    std::lock_guard lock(impl_->mutex);
    auto found = impl_->tables.find(identity);
    if (found == impl_->tables.end()) found = impl_->tables.emplace(identity, read_table(impl_->path(identity))).first;
    if (!found->second) return std::unexpected(found->second.error());
    const auto value = found->second->find(key);
    if (value == found->second->end()) return std::optional<std::string>{};
    return std::optional<std::string>{value->second};
}

std::vector<std::string> XmlCatalog::available_languages() const {
    std::lock_guard lock(impl_->mutex);
    std::set<std::string> languages{impl_->default_language};
    std::error_code error;
    if (!fs::exists(impl_->root, error)) {
        if (error) throw std::runtime_error("i18n: " + impl_->root.string() + ": " + error.message());
        return {languages.begin(), languages.end()};
    }
    // Throwing iterators keep inaccessible directories distinguishable from an
    // empty catalog. They do not silently invent a complete language listing.
    for (const auto& domain : fs::directory_iterator(impl_->root)) {
        if (!domain.is_directory() || !identifier(domain.path().filename().string())) continue;
        for (const auto& entry : fs::directory_iterator(domain.path())) {
            if (!entry.is_regular_file()) continue;
            const std::string filename = entry.path().filename().string();
            constexpr std::string_view prefix = "strings_";
            constexpr std::string_view suffix = ".xml";
            if (filename.starts_with(prefix) && filename.ends_with(suffix) &&
                filename.size() > prefix.size() + suffix.size()) {
                auto language = filename.substr(prefix.size(), filename.size() - prefix.size() - suffix.size());
                if (identifier(language)) languages.insert(std::move(language));
            }
        }
    }
    return {languages.begin(), languages.end()};
}

std::expected<void, CatalogError> XmlCatalog::reload() {
    std::lock_guard lock(impl_->mutex);
    std::map<Impl::Identity, TableResult> replacement;
    for (const auto& [identity, ignored] : impl_->tables) {
        (void)ignored;
        auto table = read_table(impl_->path(identity));
        if (!table) return std::unexpected(table.error());
        replacement.emplace(identity, std::move(table));
    }
    impl_->tables.swap(replacement);
    return {};
}

}  // namespace aria::i18n
