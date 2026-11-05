/**
 * @file strings.cppm
 * @brief Provide string, percent-encoding, and HTML-escaping helpers shared by the HTTP layer.
 */
export module mhttp:strings;

import std;

export namespace mhttp::url {

    /**
     * @brief Percent-decode a string; malformed escapes are kept verbatim.
     * @param str Encoded text; '+' is not treated as a space.
     * @return Decoded bytes.
     */
    auto url_decode(std::string_view str) -> std::string;

    /**
     * @brief Percent-encode everything except unreserved characters; spaces become '+'.
     * @param value Raw bytes.
     * @return Encoded text suitable for query strings and form bodies.
     */
    auto url_encode(std::string_view value) -> std::string;

} // namespace mhttp::url

namespace mhttp::detail {

    /**
     * @brief Transparent hash so std::string-keyed maps accept string_view lookups.
     */
    struct StringHash {
        using is_transparent = void;

        auto operator()(std::string_view value) const noexcept -> std::size_t {
            return std::hash<std::string_view>{}(value);
        }
    };

    /**
     * @brief std::string-keyed hash map with heterogeneous string_view lookup.
     * @tparam T Mapped type.
     */
    template <typename T>
    using StringMap = std::unordered_map<std::string, T, StringHash, std::equal_to<>>;

    /**
     * @brief Report whether a byte is ASCII whitespace.
     * @param c Byte to test.
     * @return True for space, tab, CR, LF, VT, and FF.
     */
    constexpr auto is_space(char c) noexcept -> bool {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
    }

    /**
     * @brief Strip leading and trailing ASCII whitespace.
     * @param value Text to trim.
     * @return View into `value` without surrounding whitespace.
     */
    constexpr auto trim(std::string_view value) noexcept -> std::string_view {
        while (!value.empty() && is_space(value.front())) {
            value.remove_prefix(1);
        }
        while (!value.empty() && is_space(value.back())) {
            value.remove_suffix(1);
        }
        return value;
    }

    /**
     * @brief Compare two strings ASCII case-insensitively.
     * @param lhs First string.
     * @param rhs Second string.
     * @return True when both are equal ignoring ASCII case.
     */
    constexpr auto iequals(std::string_view lhs, std::string_view rhs) noexcept -> bool {
        auto const lower{ [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; } };
        return lhs.size() == rhs.size() && std::ranges::equal(lhs, rhs, {}, lower, lower);
    }

    /**
     * @brief Split a string on a delimiter (port of httplib::util::split).
     * @param str Text to split.
     * @param delimiter Separator; empty returns `str` as one part.
     * @param compress Trim every part; leading delimiters are always skipped.
     * @return Views into `str`.
     */
    auto split(
        std::string_view str,
        std::string_view delimiter,
        bool compress = true
    ) -> std::vector<std::string_view> {
        if (str.empty()) {
            return {};
        }
        if (delimiter.empty()) {
            return { str };
        }
        std::vector<std::string_view> parts;
        std::size_t pos{ 0 };
        while (pos != std::string_view::npos) {
            auto const found{ str.find(delimiter, pos) };
            if (found == 0) {
                pos += delimiter.size();
                continue;
            }
            auto part{ str.substr(pos, found - pos) };
            parts.push_back(compress ? trim(part) : part);
            if (found == std::string_view::npos || found + delimiter.size() >= str.size()) {
                break;
            }
            pos = found + delimiter.size();
        }
        return parts;
    }

    /**
     * @brief Concatenate strings with a separator.
     * @param parts Strings to join.
     * @param separator Text placed between parts.
     * @return Joined string.
     */
    auto join(
        std::ranges::input_range auto const& parts,
        std::string_view separator
    ) -> std::string {
        std::string out;
        bool first{ true };
        for (auto const& part : parts) {
            if (!first) {
                out += separator;
            }
            out += part;
            first = false;
        }
        return out;
    }

    /**
     * @brief Parse a whole string as a number.
     * @tparam T Integral or floating-point type.
     * @param value Text that must be consumed entirely.
     * @return Parsed number.
     * @throws std::runtime_error If `value` is not exactly one number of type T.
     */
    template <typename T>
    requires std::integral<T> || std::floating_point<T>
    auto from_chars_strict(std::string_view value) -> T {
        T result{};
        auto const [end, ec]{ std::from_chars(value.data(), value.data() + value.size(), result) };
        if (ec != std::errc{} || end != value.data() + value.size()) {
            throw std::runtime_error{ "cannot convert '" + std::string{ value } + "'" };
        }
        return result;
    }

    /**
     * @brief Escape text for inclusion in HTML.
     * @param value Raw text.
     * @return Text with & < > " ' replaced by entities.
     */
    auto html_escape(std::string_view value) -> std::string {
        std::string out;
        out.reserve(value.size());
        for (char const c : value) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
            }
        }
        return out;
    }

    /**
     * @brief Percent-encode a path for an href, keeping '/' and unreserved characters.
     * @param value Raw path bytes.
     * @return Encoded path.
     */
    auto href_encode(std::string_view value) -> std::string {
        static constexpr std::string_view HEX{ "0123456789ABCDEF" };
        std::string out;
        out.reserve(value.size() * 2);
        for (char const c : value) {
            auto const u{ static_cast<unsigned char>(c) };
            if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
                out += c;
            } else {
                out += '%';
                out += HEX[u >> 4];
                out += HEX[u & 0xF];
            }
        }
        return out;
    }

    /**
     * @brief Convert a hex digit to its value.
     * @param c Character to convert.
     * @return Digit value, or std::nullopt when `c` is not a hex digit.
     */
    constexpr auto hex_value(char c) noexcept -> std::optional<unsigned> {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a' + 10);
        }
        if (c >= 'A' && c <= 'F') {
            return static_cast<unsigned>(c - 'A' + 10);
        }
        return std::nullopt;
    }

} // namespace mhttp::detail

namespace mhttp::url {

    auto url_decode(std::string_view str) -> std::string {
        std::string out;
        out.reserve(str.size());
        for (std::size_t i{ 0 }; i < str.size(); ++i) {
            if (str[i] == '%' && i + 2 < str.size()) {
                auto const high{ detail::hex_value(str[i + 1]) };
                auto const low{ detail::hex_value(str[i + 2]) };
                if (high && low) {
                    out += static_cast<char>((*high << 4) | *low);
                    i += 2;
                    continue;
                }
            }
            out += str[i];
        }
        return out;
    }

    auto url_encode(std::string_view value) -> std::string {
        static constexpr std::string_view HEX{ "0123456789abcdef" };
        std::string out;
        out.reserve(value.size() * 3);
        for (char const c : value) {
            auto const u{ static_cast<unsigned char>(c) };
            if (std::isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += c;
            } else if (c == ' ') {
                out += '+';
            } else {
                out += '%';
                out += HEX[u >> 4];
                out += HEX[u & 0xF];
            }
        }
        return out;
    }

} // namespace mhttp::url
