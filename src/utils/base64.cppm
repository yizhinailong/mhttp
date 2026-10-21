/**
 * @file base64.cppm
 * @brief Encode and strictly decode padded RFC 4648 Base64 for HTTP authentication.
 */
export module mhttp:utils.base64;

import std;

namespace mhttp::detail {

    /**
     * @brief Decode one byte from the standard Base64 alphabet.
     * @param byte Encoded byte.
     * @return A six-bit value, or -1 for padding, whitespace, and other invalid bytes.
     */
    constexpr auto base64_value(char byte) noexcept -> int {
        if (byte >= 'A' && byte <= 'Z') {
            return byte - 'A';
        }
        if (byte >= 'a' && byte <= 'z') {
            return byte - 'a' + 26;
        }
        if (byte >= '0' && byte <= '9') {
            return byte - '0' + 52;
        }
        if (byte == '+') {
            return 62;
        }
        return byte == '/' ? 63 : -1;
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Encode bytes using the standard alphabet with required = padding.
     * @param value Borrowed binary input, including embedded NUL bytes.
     * @return Owned Base64 text with no line wrapping.
     */
    [[nodiscard]] auto Base64Encode(std::string_view value) -> std::string {
        constexpr std::string_view ALPHABET{
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
        };
        std::string result;
        for (std::size_t index{}; index < value.size();) {
            auto const count = std::min<std::size_t>(3, value.size() - index);
            std::uint32_t block{ static_cast<unsigned char>(value[index]) };
            block <<= 16;
            if (count >= 2) {
                block |= static_cast<std::uint32_t>(static_cast<unsigned char>(value[index + 1])) << 8;
            }
            if (count == 3) {
                block |= static_cast<unsigned char>(value[index + 2]);
            }
            result.push_back(ALPHABET[(block >> 18) & 63]);
            result.push_back(ALPHABET[(block >> 12) & 63]);
            result.push_back(count >= 2 ? ALPHABET[(block >> 6) & 63] : '=');
            result.push_back(count == 3 ? ALPHABET[block & 63] : '=');
            index += count;
        }
        return result;
    }

    /**
     * @brief Decode canonical padded RFC 4648 Base64.
     * @param value Borrowed encoded input; whitespace and the URL-safe alphabet are rejected.
     * @return Owned binary bytes, or nullopt for invalid length, alphabet, padding, or pad bits.
     * @note An empty input succeeds with an empty string. Missing required padding is rejected.
     */
    [[nodiscard]] auto Base64Decode(std::string_view value) -> std::optional<std::string> {
        if (value.size() % 4 != 0) {
            return std::nullopt;
        }
        std::string result;
        result.reserve(value.size() / 4 * 3);
        for (std::size_t index{}; index < value.size(); index += 4) {
            int const first = detail::base64_value(value[index]);
            int const second = detail::base64_value(value[index + 1]);
            if (first < 0 || second < 0) {
                return std::nullopt;
            }
            bool const last = value.size() - index == 4;
            if (value[index + 2] == '=') {
                if (!last || value[index + 3] != '=' || (second & 15) != 0) {
                    return std::nullopt;
                }
                result.push_back(static_cast<char>((first << 2) | (second >> 4)));
                continue;
            }
            int const third = detail::base64_value(value[index + 2]);
            if (third < 0) {
                return std::nullopt;
            }
            result.push_back(static_cast<char>((first << 2) | (second >> 4)));
            result.push_back(static_cast<char>((second << 4) | (third >> 2)));
            if (value[index + 3] == '=') {
                if (!last || (third & 3) != 0) {
                    return std::nullopt;
                }
                continue;
            }
            int const fourth = detail::base64_value(value[index + 3]);
            if (fourth < 0) {
                return std::nullopt;
            }
            result.push_back(static_cast<char>((third << 6) | fourth));
        }
        return result;
    }

} // namespace mhttp
