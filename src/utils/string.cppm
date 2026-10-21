/**
 * @file string.cppm
 * @brief Provide locale-independent HTTP character and string utilities.
 */
export module mhttp:utils.string;

import std;

export namespace mhttp::utils {

    /**
     * @brief Convert an ASCII capital letter to lowercase without consulting the locale.
     * @param value Byte to convert; all other bytes are preserved.
     * @return Converted byte.
     */
    [[nodiscard]] constexpr auto ascii_lower(char value) noexcept -> char {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    }

    /**
     * @brief Compare byte strings using ASCII case folding.
     * @param left First borrowed string.
     * @param right Second borrowed string.
     * @return Whether sizes and folded bytes match.
     */
    [[nodiscard]] constexpr auto iequals(
        std::string_view left,
        std::string_view right
    ) noexcept -> bool {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index{}; index < left.size(); ++index) {
            if (ascii_lower(left[index]) != ascii_lower(right[index])) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Hash strings iteratively with the same ASCII folding as iequals.
     * @note Transparent lookup borrows strings and allocates no temporary key.
     */
    struct CaseInsensitiveHash {
        using is_transparent = void; ///< Enable heterogeneous unordered-container lookup.

        /**
         * @brief Hash every folded byte, including embedded NUL bytes.
         * @param value Borrowed key.
         * @return Case-insensitive hash value.
         */
        [[nodiscard]] auto operator()(std::string_view value) const noexcept -> std::size_t {
            std::uint64_t hash{ 14695981039346656037ULL };
            for (char byte : value) {
                hash ^= static_cast<unsigned char>(ascii_lower(byte));
                hash *= 1099511628211ULL;
            }
            return static_cast<std::size_t>(hash);
        }
    };

    /**
     * @brief Compare heterogeneous keys with locale-independent ASCII folding.
     */
    struct CaseInsensitiveEqual {
        using is_transparent = void; ///< Enable heterogeneous unordered-container lookup.

        /**
         * @brief Compare borrowed keys.
         * @param left First key.
         * @param right Second key.
         * @return Whether the keys compare equal ignoring ASCII case.
         */
        [[nodiscard]] auto operator()(
            std::string_view left,
            std::string_view right
        ) const noexcept -> bool {
            return iequals(left, right);
        }
    };

    /**
     * @brief Make an owned copy with ASCII letters converted to lowercase.
     * @param value Borrowed source bytes.
     * @return Independently owned lowercase text.
     */
    [[nodiscard]] auto ToLower(std::string_view value) -> std::string {
        std::string result{ value };
        for (char& byte : result) {
            byte = ascii_lower(byte);
        }
        return result;
    }

    /**
     * @brief Recognize HTTP optional whitespace: SP and HTAB only.
     * @param value Byte to classify.
     * @return Whether the byte is optional whitespace.
     */
    [[nodiscard]] constexpr auto is_ows(char value) noexcept -> bool {
        return value == ' ' || value == '\t';
    }

    /**
     * @brief Remove leading and trailing HTTP optional whitespace.
     * @param value Borrowed input bytes.
     * @return Subview valid for as long as the original storage remains valid.
     */
    [[nodiscard]] constexpr auto trim_ows(std::string_view value) noexcept -> std::string_view {
        while (!value.empty() && is_ows(value.front())) {
            value.remove_prefix(1);
        }
        while (!value.empty() && is_ows(value.back())) {
            value.remove_suffix(1);
        }
        return value;
    }

    /**
     * @brief Split a byte string without copying the segments.
     * @param value Borrowed source; an empty source produces no segments.
     * @param delimiter Separator byte.
     * @param compress If true, trim OWS and omit empty segments; otherwise preserve empty segments.
     * @return Views whose storage belongs to the caller and must outlive their use.
     */
    [[nodiscard]] auto split(
        std::string_view value,
        char delimiter,
        bool compress = false
    ) -> std::vector<std::string_view> {
        std::vector<std::string_view> result;
        if (value.empty()) {
            return result;
        }
        for (;;) {
            auto const position = value.find(delimiter);
            auto part = value.substr(0, position);
            if (compress) {
                part = trim_ows(part);
            }
            if (!compress || !part.empty()) {
                result.push_back(part);
            }
            if (position == std::string_view::npos) {
                return result;
            }
            value.remove_prefix(position + 1);
        }
    }

    /**
     * @brief Parse an integer only if every byte is consumed and the value fits.
     * @tparam T Integer type supported by std::from_chars; bool is not supported.
     * @param value Borrowed digits; whitespace and a leading plus are rejected.
     * @param base Numeric base in [2, 36]; no base prefix is consumed.
     * @return Parsed value or nullopt for empty, malformed, or overflowing input.
     */
    template <typename T>
        requires std::integral<T> && requires(char const* begin, char const* end, T& value) {
            std::from_chars(begin, end, value);
        }
    [[nodiscard]] auto parse_integer(
        std::string_view value,
        int base = 10
    ) noexcept -> std::optional<T> {
        if (value.empty() || base < 2 || base > 36) {
            return std::nullopt;
        }
        T result{};
        auto const end = value.data() + value.size();
        auto const parsed = std::from_chars(value.data(), end, result, base);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            return std::nullopt;
        }
        return result;
    }

    /**
     * @brief Classify RFC 9110 tchar bytes without locale-dependent character functions.
     * @param value Byte to classify.
     * @return Whether the byte may occur in a token.
     */
    [[nodiscard]] constexpr auto is_token_char(char value) noexcept -> bool {
        return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z')
            || std::string_view{ "!#$%&'*+-.^_`|~" }.find(value) != std::string_view::npos;
    }

    /**
     * @brief Validate a nonempty HTTP field name.
     * @param value Borrowed field name.
     * @return Whether every byte is tchar and at least one byte exists.
     */
    [[nodiscard]] constexpr auto is_field_name(std::string_view value) noexcept -> bool {
        return !value.empty() && std::ranges::all_of(value, is_token_char);
    }

    /**
     * @brief Validate an HTTP field value after surrounding OWS has been removed.
     * @param value Borrowed field value; empty values and obs-text bytes are permitted.
     * @return False for surrounding OWS, CR, LF, NUL, DEL, or other disallowed controls.
     */
    [[nodiscard]] constexpr auto is_field_value(std::string_view value) noexcept -> bool {
        if (!value.empty() && (is_ows(value.front()) || is_ows(value.back()))) {
            return false;
        }
        return std::ranges::all_of(value, [](unsigned char byte) {
            return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
        });
    }

} // namespace mhttp::utils
