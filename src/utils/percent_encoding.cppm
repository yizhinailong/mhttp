/**
 * @file percent_encoding.cppm
 * @brief Encode and decode URI bytes with explicit path, query, and complete-URI modes.
 */
export module mhttp:utils.percent;

import std;

namespace mhttp::detail {

    /**
     * @brief Convert one ASCII hexadecimal digit.
     * @param byte Byte to decode.
     * @return Value in [0, 15], or -1 for an invalid digit.
     */
    constexpr auto hex_value(char byte) noexcept -> int {
        if (byte >= '0' && byte <= '9') {
            return byte - '0';
        }
        if (byte >= 'A' && byte <= 'F') {
            return byte - 'A' + 10;
        }
        if (byte >= 'a' && byte <= 'f') {
            return byte - 'a' + 10;
        }
        return -1;
    }

    /**
     * @brief Identify reserved bytes preserved when operating on an entire URI.
     * @param byte Byte to classify.
     * @return Whether the byte has delimiter semantics in a complete URI.
     */
    constexpr auto is_uri_reserved(char byte) noexcept -> bool {
        return std::string_view{ ";,/?:@&=+$#" }.contains(byte);
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Select percent-encoding rules and the interpretation of plus signs.
     */
    enum class UrlEncoding {
        PathSegment, ///< Encode separators; represent spaces as %20 and preserve literal plus on decode.
        QueryComponent, ///< Encode separators; represent spaces as + and encode literal plus as %2B.
        Uri, ///< Preserve URI delimiters, including reserved percent escapes when decoding.
    };

    /**
     * @brief Encode arbitrary bytes using uppercase percent escapes.
     * @param value Borrowed source bytes; UTF-8 is encoded byte by byte.
     * @param mode Encoding rules; the common safe set includes -_.!~*'() and ASCII alphanumerics.
     * @return Independently owned encoded bytes.
     */
    [[nodiscard]] auto UrlEncode(
        std::string_view value,
        UrlEncoding mode = UrlEncoding::PathSegment
    ) -> std::string {
        constexpr std::string_view HEX{ "0123456789ABCDEF" };
        std::string result;
        result.reserve(value.size());
        for (unsigned char byte : value) {
            bool const safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z')
                || (byte >= '0' && byte <= '9')
                || std::string_view{ "-_.!~*'()" }.contains(static_cast<char>(byte));
            if (safe || (mode == UrlEncoding::Uri && detail::is_uri_reserved(byte))) {
                result.push_back(static_cast<char>(byte));
            } else if (byte == ' ' && mode == UrlEncoding::QueryComponent) {
                result.push_back('+');
            } else {
                result.push_back('%');
                result.push_back(HEX[byte >> 4]);
                result.push_back(HEX[byte & 15]);
            }
        }
        return result;
    }

    /**
     * @brief Decode percent escapes without assuming UTF-8 or NUL-terminated input.
     * @param value Borrowed encoded bytes.
     * @param mode Query mode converts + to SP; URI mode keeps escaped reserved delimiters intact.
     * @return Owned decoded bytes, including embedded NULs; malformed escape triplets stay literal.
     * @note Nonstandard %uXXXX sequences are preserved, never decoded as Unicode.
     */
    [[nodiscard]] auto UrlDecode(
        std::string_view value,
        UrlEncoding mode = UrlEncoding::PathSegment
    ) -> std::string {
        std::string result;
        result.reserve(value.size());
        for (std::size_t index{}; index < value.size();) {
            char const byte = value[index];
            if (byte == '%') {
                auto const remaining = value.size() - index;
                auto const count = std::min<std::size_t>(3, remaining);
                if (remaining >= 3) {
                    int const high = detail::hex_value(value[index + 1]);
                    int const low = detail::hex_value(value[index + 2]);
                    if (high >= 0 && low >= 0) {
                        char const decoded = static_cast<char>((high << 4) | low);
                        if (mode == UrlEncoding::Uri && detail::is_uri_reserved(decoded)) {
                            result.append(value.substr(index, 3));
                        } else {
                            result.push_back(decoded);
                        }
                        index += 3;
                        continue;
                    }
                }
                result.append(value.substr(index, count));
                index += count;
            } else {
                result.push_back(byte == '+' && mode == UrlEncoding::QueryComponent ? ' ' : byte);
                ++index;
            }
        }
        return result;
    }

} // namespace mhttp
