/**
 * @file url.cppm
 * @brief Parse absolute HTTP URLs and resolve relative references (RFC 3986), without Boost.URL.
 */
module;

#include <boost/system/error_code.hpp>

export module mhttp:url;

import std;
import :config;
import :error;
import :strings;

export namespace mhttp::url {

    /**
     * @brief Transport of a connection; an enum instead of a bool so it cannot be passed by mistake.
     */
    enum class Scheme : unsigned short {
        Plain, ///< Cleartext HTTP, default port 80.
        Tls,   ///< HTTPS, default port 443.
    };

    /**
     * @brief Return the URL scheme name of a transport.
     * @param scheme Transport.
     * @return "http" or "https".
     */
    constexpr auto to_string(Scheme scheme) noexcept -> std::string_view {
        return scheme == Scheme::Tls ? "https" : "http";
    }

    /**
     * @brief Return the default port of a transport.
     * @param scheme Transport.
     * @return 80 or 443.
     */
    constexpr auto default_port(Scheme scheme) noexcept -> std::uint16_t {
        return scheme == Scheme::Tls ? 443 : 80;
    }

    /**
     * @brief Components of an absolute URL; path, query, and fragment stay percent-encoded.
     */
    struct UrlInfo {
        std::string scheme;      ///< Scheme as written, e.g. "http" or "HTTPS".
        std::string host;        ///< Host name or address; IPv6 literals without brackets.
        std::uint16_t port{ 0 }; ///< Explicit port; 0 when absent.
        std::string path;        ///< Encoded path; may be empty.
        std::string query;       ///< Encoded query without '?'.
        std::string fragment;    ///< Encoded fragment without '#'.
        bool has_query{ false }; ///< A '?' was present, even with an empty query.

        [[nodiscard]] auto HasPort() const noexcept -> bool {
            return port != 0;
        }

        /**
         * @brief Report whether the scheme implies TLS.
         * @return True for https and wss, case-insensitively.
         */
        [[nodiscard]] auto IsTls() const noexcept -> bool {
            return detail::iequals(scheme, "https") || detail::iequals(scheme, "wss");
        }

        [[nodiscard]] auto Transport() const noexcept -> Scheme {
            return IsTls() ? Scheme::Tls : Scheme::Plain;
        }

        /**
         * @brief Return the explicit port, or the scheme's default port.
         * @return Port to connect to.
         */
        [[nodiscard]] auto EffectivePort() const noexcept -> std::uint16_t {
            return port != 0 ? port : default_port(Transport());
        }

        /**
         * @brief Return the origin-form request target.
         * @param include_fragment Append "#fragment"; request lines never carry it (RFC 9110).
         * @return Path ("/" when empty) plus "?query" and optionally "#fragment".
         */
        [[nodiscard]] auto Target(bool include_fragment = false) const -> std::string {
            std::string out{ path.empty() ? "/" : path };
            if (has_query) {
                out += '?';
                out += query;
            }
            if (include_fragment && !fragment.empty()) {
                out += '#';
                out += fragment;
            }
            return out;
        }
    };

    /**
     * @brief Parse an absolute URL with an authority ("scheme://host[:port]...").
     * @param text URL text.
     * @return Components, or Error::InvalidUrl for a missing scheme or host, a bad port, or
     *         whitespace and control characters.
     */
    auto parse_url(std::string_view text) -> Result<UrlInfo>;

    /**
     * @brief Resolve a possibly relative reference against a request target (RFC 3986 §5.2).
     * @param base_target Absolute path with optional query, e.g. "/a/b?x=1".
     * @param location Reference such as "c", "../d", "?q=1", "/e", or a full URL.
     * @return Resulting path and query on the same authority; the fragment is dropped.
     */
    auto resolve(std::string_view base_target, std::string_view location) -> std::string;

    /**
     * @brief Build a Host header value.
     * @param host Host name or address; IPv6 literals get brackets.
     * @param port Port; omitted when it is the scheme's default.
     * @param scheme Transport.
     * @return "host[:port]".
     */
    auto make_host_value(std::string_view host, std::uint16_t port, Scheme scheme) -> std::string;

    /**
     * @brief Build "scheme://host[:port]target".
     * @param host Host name or address.
     * @param port Port; omitted when it is the scheme's default.
     * @param scheme Transport.
     * @param target Optional request target.
     * @return URL text.
     */
    auto make_url_value(
        std::string_view host,
        std::uint16_t port,
        Scheme scheme,
        std::string_view target = {}
    ) -> std::string;

} // namespace mhttp::url

namespace mhttp::url {

    namespace {

        /**
         * @brief Components of a URI reference split per RFC 3986 Appendix B.
         */
        struct Reference {
            std::optional<std::string_view> scheme;    ///< Scheme without ':'.
            std::optional<std::string_view> authority; ///< Authority without "//".
            std::string_view path;                     ///< Path; may be empty.
            std::optional<std::string_view> query;     ///< Query without '?'.
        };

        /**
         * @brief Split a reference into scheme, authority, path, and query; ignore the fragment.
         * @param text Reference text.
         * @return Components viewing `text`.
         */
        auto split_reference(std::string_view text) -> Reference {
            Reference out;
            if (auto const hash{ text.find('#') }; hash != std::string_view::npos) {
                text = text.substr(0, hash);
            }
            // A scheme is letters/digits/+/-/. before the first ':' that precedes any '/', '?'.
            if (auto const colon{ text.find_first_of(":/?") }; colon != std::string_view::npos && colon > 0 && text[colon] == ':') {
                auto const candidate{ text.substr(0, colon) };
                bool const valid{ std::isalpha(static_cast<unsigned char>(candidate.front())) != 0 && std::ranges::all_of(candidate, [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '.';
                }) };
                if (valid) {
                    out.scheme = candidate;
                    text.remove_prefix(colon + 1);
                }
            }
            if (text.starts_with("//")) {
                text.remove_prefix(2);
                auto const end{ text.find_first_of("/?") };
                out.authority = text.substr(0, end);
                text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
            }
            if (auto const question{ text.find('?') }; question != std::string_view::npos) {
                out.query = text.substr(question + 1);
                text = text.substr(0, question);
            }
            out.path = text;
            return out;
        }

        /**
         * @brief Remove "." and ".." segments (RFC 3986 §5.2.4).
         * @param input Path to normalize.
         * @return Normalized path.
         */
        auto remove_dot_segments(std::string_view input) -> std::string {
            std::string output;
            while (!input.empty()) {
                if (input.starts_with("../")) {
                    input.remove_prefix(3);
                } else if (input.starts_with("./")) {
                    input.remove_prefix(2);
                } else if (input.starts_with("/./")) {
                    input.remove_prefix(2);
                } else if (input == "/.") {
                    input = "/";
                } else if (input.starts_with("/../") || input == "/..") {
                    input = input.size() == 3 ? std::string_view{ "/" } : input.substr(3);
                    if (auto const slash{ output.rfind('/') }; slash != std::string::npos) {
                        output.erase(slash);
                    } else {
                        output.clear();
                    }
                } else if (input == "." || input == "..") {
                    input = {};
                } else {
                    auto const next{ input.find('/', input.front() == '/' ? 1 : 0) };
                    output += input.substr(0, next);
                    input = next == std::string_view::npos ? std::string_view{} : input.substr(next);
                }
            }
            return output;
        }

        /**
         * @brief Report whether text holds whitespace or control characters, which URLs exclude.
         * @param text Text to check.
         * @return True when a byte is <= 0x20 or 0x7F.
         */
        auto has_forbidden_char(std::string_view text) -> bool {
            return std::ranges::any_of(text, [](char c) {
                auto const byte{ static_cast<unsigned char>(c) };
                return byte <= 0x20 || byte == 0x7F;
            });
        }

    } // namespace

    auto parse_url(std::string_view text) -> Result<UrlInfo> {
        auto const invalid{ std::unexpected{ make_error_code(Error::InvalidUrl) } };
        if (text.empty() || has_forbidden_char(text)) {
            return invalid;
        }
        UrlInfo out;
        if (auto const hash{ text.find('#') }; hash != std::string_view::npos) {
            out.fragment = std::string{ text.substr(hash + 1) };
            text = text.substr(0, hash);
        }
        auto const reference{ split_reference(text) };
        if (!reference.scheme || !reference.authority) {
            return invalid;
        }
        out.scheme = std::string{ *reference.scheme };

        auto authority{ *reference.authority };
        if (auto const at{ authority.rfind('@') }; at != std::string_view::npos) {
            authority.remove_prefix(at + 1); // userinfo is never sent
        }
        std::string_view port_text;
        if (authority.starts_with('[')) {
            auto const close{ authority.find(']') };
            if (close == std::string_view::npos) {
                return invalid;
            }
            out.host = std::string{ authority.substr(1, close - 1) };
            auto const rest{ authority.substr(close + 1) };
            if (!rest.empty()) {
                if (rest.front() != ':') {
                    return invalid;
                }
                port_text = rest.substr(1);
            }
        } else {
            auto const colon{ authority.rfind(':') };
            out.host = std::string{ authority.substr(0, colon) };
            if (colon != std::string_view::npos) {
                port_text = authority.substr(colon + 1);
            }
        }
        if (out.host.empty()) {
            return invalid;
        }
        if (!port_text.empty()) {
            std::uint32_t port{ 0 };
            auto const [end, ec]{ std::from_chars(port_text.data(), port_text.data() + port_text.size(), port) };
            if (ec != std::errc{} || end != port_text.data() + port_text.size() || port == 0 || port > 65535) {
                return invalid;
            }
            out.port = static_cast<std::uint16_t>(port);
        }
        out.path = std::string{ reference.path };
        if (reference.query) {
            out.has_query = true;
            out.query = std::string{ *reference.query };
        }
        return out;
    }

    auto resolve(std::string_view base_target, std::string_view location) -> std::string {
        auto const base{ split_reference(base_target) };
        auto const reference{ split_reference(location) };

        std::string path;
        std::optional<std::string_view> query{ reference.query };
        if (reference.scheme || reference.authority) {
            path = remove_dot_segments(reference.path);
        } else if (reference.path.empty()) {
            path = std::string{ base.path };
            if (!query) {
                query = base.query;
            }
        } else if (reference.path.starts_with('/')) {
            path = remove_dot_segments(reference.path);
        } else {
            // Merge (RFC 3986 §5.2.3): the base has an authority, so an empty base path is "/".
            auto const slash{ base.path.rfind('/') };
            std::string merged{ slash == std::string_view::npos ? std::string{ "/" } : std::string{ base.path.substr(0, slash + 1) } };
            merged += reference.path;
            path = remove_dot_segments(merged);
        }
        if (path.empty() || path.front() != '/') {
            path.insert(path.begin(), '/');
        }
        if (query) {
            path += '?';
            path += *query;
        }
        return path;
    }

    auto make_host_value(std::string_view host, std::uint16_t port, Scheme scheme) -> std::string {
        std::string out;
        if (host.contains(':') && !host.starts_with('[')) {
            out = std::format("[{}]", host);
        } else {
            out = std::string{ host };
        }
        if (port != default_port(scheme)) {
            out += std::format(":{}", port);
        }
        return out;
    }

    auto make_url_value(
        std::string_view host,
        std::uint16_t port,
        Scheme scheme,
        std::string_view target
    ) -> std::string {
        return std::format("{}://{}{}", to_string(scheme), make_host_value(host, port, scheme), target);
    }

} // namespace mhttp::url
