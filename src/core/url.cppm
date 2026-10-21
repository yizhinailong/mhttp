/**
 * @file url.cppm
 * @brief Parse owned HTTP URLs and resolve relative redirect references without a network backend.
 */
export module mhttp:core.url;

import std;
import :utils.string;
import :utils.percent;
export import :core.error;

namespace mhttp::detail {

    /**
     * @brief Validate the lexical form of a URI scheme.
     * @param value Borrowed scheme, without the colon.
     * @return Whether the scheme follows the ASCII URI grammar.
     */
    constexpr auto is_url_scheme(std::string_view value) noexcept -> bool {
        auto const alpha = [](char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        };
        return !value.empty() && alpha(value.front()) && std::ranges::all_of(value, [alpha](char byte) {
            return alpha(byte) || (byte >= '0' && byte <= '9') || byte == '+' || byte == '-' || byte == '.';
        });
    }

    /**
     * @brief Reject unencoded whitespace, controls, and backslash-based host ambiguity.
     * @param value Borrowed URL or reference.
     * @return Whether the input contains no unsafe literal bytes.
     */
    constexpr auto has_safe_url_bytes(std::string_view value) noexcept -> bool {
        return std::ranges::all_of(value, [](unsigned char byte) {
            return byte > 0x20 && byte != 0x7f && byte != '\\';
        });
    }

    /**
     * @brief Validate a dotted IPv4 tail inside an IPv6 literal.
     * @param value Borrowed dotted decimal address.
     * @return Whether exactly four decimal octets in [0, 255] are present.
     */
    auto is_url_ipv4(std::string_view value) noexcept -> bool {
        unsigned count{};
        for (;;) {
            auto const separator = value.find('.');
            auto const part = value.substr(0, separator);
            if (part.empty() || part.size() > 3 || (part.size() > 1 && part.front() == '0')) {
                return false;
            }
            auto const octet = utils::parse_integer<unsigned>(part);
            if (!octet || *octet > 255) { return false; }
            ++count;
            if (separator == std::string_view::npos) { return count == 4; }
            value.remove_prefix(separator + 1);
        }
    }

    /**
     * @brief Validate a bracketed IPv6 address without a platform socket parser.
     * @param value Bare address without brackets; zone identifiers are not supported.
     * @return Whether the literal contains eight groups, allowing one :: compression and an IPv4 tail.
     */
    auto is_url_ipv6(std::string_view value) noexcept -> bool {
        if (value.empty()) { return false; }
        bool compressed = value.starts_with("::");
        std::size_t index = compressed ? 2 : 0;
        unsigned groups{};
        while (index < value.size()) {
            auto const separator = value.find(':', index);
            auto const end = separator == std::string_view::npos ? value.size() : separator;
            auto const part = value.substr(index, end - index);
            if (part.empty()) { return false; }
            if (part.contains('.')) {
                if (end != value.size() || !is_url_ipv4(part)) { return false; }
                groups += 2;
            } else {
                if (part.size() > 4 || !std::ranges::all_of(part, [](char byte) {
                    return hex_value(byte) >= 0;
                })) { return false; }
                ++groups;
            }
            if (end == value.size()) { break; }
            if (end + 1 < value.size() && value[end + 1] == ':') {
                if (compressed) { return false; }
                compressed = true;
                index = end + 2;
            } else {
                index = end + 1;
                if (index == value.size()) { return false; }
            }
        }
        return compressed ? groups < 8 : groups == 8;
    }

    /**
     * @brief Validate a registered host name after percent decoding.
     * @param value Borrowed host bytes.
     * @return Whether the host is nonempty and contains only safe ASCII reg-name bytes.
     */
    constexpr auto is_url_host(std::string_view value) noexcept -> bool {
        return !value.empty() && std::ranges::all_of(value, [](char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
                || (byte >= '0' && byte <= '9') || std::string_view{ "-._~!$&'()*+,;=" }.contains(byte);
        });
    }

    /**
     * @brief Remove literal dot segments using RFC 3986 section 5.2.4.
     * @param path Borrowed encoded path; percent-encoded dots and separators stay encoded.
     * @return Owned normalized path, preserving significant empty segments and trailing slashes.
     */
    auto remove_url_dot_segments(std::string_view path) -> std::string {
        std::string result;
        while (!path.empty()) {
            if (path.starts_with("../")) { path.remove_prefix(3); }
            else if (path.starts_with("./")) { path.remove_prefix(2); }
            else if (path.starts_with("/./")) { path.remove_prefix(2); }
            else if (path == "/.") { path = "/"; }
            else if (path.starts_with("/../") || path == "/..") {
                path = path == "/.." ? std::string_view{ "/" } : path.substr(3);
                auto const slash = result.find_last_of('/');
                result.resize(slash == std::string::npos ? 0 : slash);
            } else if (path == "." || path == "..") { path = {}; }
            else {
                auto const end = path.find('/', path.starts_with('/') ? 1 : 0);
                auto const count = end == std::string_view::npos ? path.size() : end;
                result.append(path.substr(0, count));
                path.remove_prefix(count);
            }
        }
        return result;
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Construct a Host value from a previously validated host and port.
     * @param host Bare hostname or IPv6 literal, optionally already enclosed in brackets.
     * @param port TCP port, including zero if explicitly supplied.
     * @param scheme Scheme used to choose the default port; http and https are recognized.
     * @return Owned authority with IPv6 brackets and a port only when nondefault.
     */
    [[nodiscard]] auto MakeHostValue(
        std::string_view host,
        std::uint16_t port = 80,
        std::string_view scheme = "http"
    ) -> std::string {
        std::string result;
        if (host.contains(':') && !(host.starts_with('[') && host.ends_with(']'))) {
            result = '[';
            result += host;
            result += ']';
        } else { result = host; }
        bool const default_port = (utils::iequals(scheme, "http") && port == 80)
            || (utils::iequals(scheme, "https") && port == 443);
        if (!default_port) { result += ':' + std::to_string(port); }
        return result;
    }

    /**
     * @brief Own a validated absolute HTTP URL and its encoded path, query, and fragment.
     * @note Returned component views expire on destruction or assignment. HTTPS and other schemes
     *       return UnsupportedScheme; URL userinfo, IPvFuture, and IPv6 zone identifiers are rejected.
     */
    class Url {
    private:
        std::string m_scheme{ "http" }; ///< Normalized supported scheme.
        std::string m_host; ///< Normalized bare host, without IPv6 brackets.
        std::uint16_t m_port{ 80 }; ///< Explicit or scheme-default TCP port.
        std::string m_path{ "/" }; ///< Encoded absolute path.
        std::string m_query; ///< Encoded query without its marker.
        std::string m_fragment; ///< Encoded fragment without its marker.
        bool m_has_query{}; ///< Distinguish no query from an explicitly empty query.
        bool m_has_fragment{}; ///< Distinguish no fragment from an explicitly empty fragment.

        /**
         * @brief Initialize storage used only by the validating factories.
         */
        Url() = default;

    public:
        /**
         * @brief Parse and copy an absolute HTTP URL without resolving its hostname.
         * @param text Borrowed URL, with an explicit scheme and authority.
         * @return Validated owned URL, InvalidUrl for syntax errors, or UnsupportedScheme.
         * @note Empty paths become /. An explicit port must contain decimal digits only and fit uint16_t.
         */
        [[nodiscard]] static auto Parse(std::string_view text) -> Result<Url> {
            if (!detail::has_safe_url_bytes(text)) { return std::unexpected{ Error::InvalidUrl }; }
            auto const colon = text.find(':');
            if (colon == std::string_view::npos || !detail::is_url_scheme(text.substr(0, colon))) {
                return std::unexpected{ Error::InvalidUrl };
            }
            auto const scheme = utils::ToLower(text.substr(0, colon));
            if (scheme != "http") { return std::unexpected{ Error::UnsupportedScheme }; }
            text.remove_prefix(colon + 1);
            if (!text.starts_with("//")) { return std::unexpected{ Error::InvalidUrl }; }
            text.remove_prefix(2);
            auto const end = text.find_first_of("/?#");
            auto const authority = text.substr(0, end);
            if (authority.empty() || authority.contains('@')) {
                return std::unexpected{ Error::InvalidUrl };
            }
            Url result;
            std::string_view host;
            std::string_view port;
            bool has_port{};
            if (authority.starts_with('[')) {
                auto const close = authority.find(']');
                if (close == std::string_view::npos) { return std::unexpected{ Error::InvalidUrl }; }
                host = authority.substr(1, close - 1);
                if (!detail::is_url_ipv6(host)) { return std::unexpected{ Error::InvalidUrl }; }
                auto const tail = authority.substr(close + 1);
                if (!tail.empty()) {
                    if (!tail.starts_with(':')) { return std::unexpected{ Error::InvalidUrl }; }
                    port = tail.substr(1);
                    has_port = true;
                }
            } else {
                auto const separator = authority.find(':');
                host = authority.substr(0, separator);
                if (separator != std::string_view::npos) {
                    has_port = true;
                    port = authority.substr(separator + 1);
                }
                auto const decoded = UrlDecode(host);
                if (!detail::is_url_host(decoded)) { return std::unexpected{ Error::InvalidUrl }; }
                result.m_host = utils::ToLower(decoded);
            }
            if (result.m_host.empty()) { result.m_host = utils::ToLower(host); }
            if (has_port) {
                auto const number = utils::parse_integer<std::uint16_t>(port);
                if (!number) { return std::unexpected{ Error::InvalidUrl }; }
                result.m_port = *number;
            }
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
            auto const hash = text.find('#');
            if (hash != std::string_view::npos) {
                result.m_has_fragment = true;
                result.m_fragment = text.substr(hash + 1);
                text = text.substr(0, hash);
            }
            auto const question = text.find('?');
            if (question != std::string_view::npos) {
                result.m_has_query = true;
                result.m_query = text.substr(question + 1);
                text = text.substr(0, question);
            }
            if (!text.empty()) { result.m_path = text; }
            return result;
        }

        /**
         * @brief Resolve an HTTP redirect reference following RFC 3986 section 5.2.
         * @param base Validated absolute HTTP base URL.
         * @param location Borrowed absolute, authority-relative, path-relative, or empty reference.
         * @return Owned resolved HTTP URL; unsupported schemes and invalid authorities return errors.
         * @note Query and fragment contents are preserved verbatim; dot removal only affects the path.
         */
        [[nodiscard]] static auto Resolve(
            Url const& base,
            std::string_view location
        ) -> Result<Url> {
            if (!detail::has_safe_url_bytes(location)) { return std::unexpected{ Error::InvalidUrl }; }
            auto const first_delimiter = location.find_first_of("/?#");
            auto const colon = location.find(':');
            if (location.starts_with("//")
                || (colon != std::string_view::npos && colon < first_delimiter)) {
                auto resolved = Parse(location.starts_with("//") ? base.m_scheme + ':' + std::string{ location }
                                                               : std::string{ location });
                if (resolved) { resolved->m_path = detail::remove_url_dot_segments(resolved->m_path); }
                return resolved;
            }
            Url result = base;
            auto const hash = location.find('#');
            result.m_has_fragment = hash != std::string_view::npos;
            result.m_fragment = result.m_has_fragment ? location.substr(hash + 1) : std::string_view{};
            location = location.substr(0, hash);
            auto const question = location.find('?');
            bool const has_query = question != std::string_view::npos;
            auto const path = location.substr(0, question);
            if (!path.empty()) {
                if (path.starts_with('/')) {
                    result.m_path = detail::remove_url_dot_segments(path);
                } else {
                    auto const slash = base.m_path.find_last_of('/');
                    result.m_path = detail::remove_url_dot_segments(base.m_path.substr(0, slash + 1) + std::string{ path });
                }
            }
            if (has_query || !path.empty()) {
                result.m_has_query = has_query;
                result.m_query = has_query ? location.substr(question + 1) : std::string_view{};
            }
            return result;
        }

        /**
         * @brief Borrow the normalized scheme.
         * @return View valid until assignment or destruction.
         */
        [[nodiscard]] auto Scheme() const noexcept -> std::string_view { return m_scheme; }

        /**
         * @brief Borrow the normalized host without IPv6 brackets.
         * @return View valid until assignment or destruction.
         */
        [[nodiscard]] auto Host() const noexcept -> std::string_view { return m_host; }

        /**
         * @brief Obtain the explicit or default TCP port.
         * @return TCP port.
         */
        [[nodiscard]] auto Port() const noexcept -> std::uint16_t { return m_port; }

        /**
         * @brief Borrow the encoded absolute path.
         * @return Nonempty path view valid until assignment or destruction.
         */
        [[nodiscard]] auto Path() const noexcept -> std::string_view { return m_path; }

        /**
         * @brief Borrow query bytes without a leading marker.
         * @return View valid until assignment or destruction.
         */
        [[nodiscard]] auto Query() const noexcept -> std::string_view { return m_query; }

        /**
         * @brief Borrow fragment bytes without a leading marker.
         * @return View valid until assignment or destruction; fragments never enter Target.
         */
        [[nodiscard]] auto Fragment() const noexcept -> std::string_view { return m_fragment; }

        /**
         * @brief Distinguish an empty query from no query.
         * @return Whether a query marker was present.
         */
        [[nodiscard]] auto HasQuery() const noexcept -> bool { return m_has_query; }

        /**
         * @brief Distinguish an empty fragment from no fragment.
         * @return Whether a fragment marker was present.
         */
        [[nodiscard]] auto HasFragment() const noexcept -> bool { return m_has_fragment; }

        /**
         * @brief Build the origin-form request target, excluding any fragment.
         * @return Owned encoded path with its query marker and query when present.
         */
        [[nodiscard]] auto Target() const -> std::string {
            return m_has_query ? m_path + '?' + m_query : m_path;
        }

        /**
         * @brief Recompose an absolute URL with normalized authority and preserved components.
         * @return Owned URL, including any fragment marker and fragment.
         */
        [[nodiscard]] auto ToString() const -> std::string {
            auto result = m_scheme + "://" + MakeHostValue(m_host, m_port, m_scheme) + Target();
            if (m_has_fragment) { result += '#' + m_fragment; }
            return result;
        }
    };

} // namespace mhttp
