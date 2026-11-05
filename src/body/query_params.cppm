/**
 * @file query_params.cppm
 * @brief Provide an ordered multimap of URL query or form-urlencoded parameters.
 */
export module mhttp:query_params;

import std;
import :strings;

export namespace mhttp {

    /**
     * @brief Ordered key/value parameters; repeated keys keep insertion order.
     */
    class QueryParams {
    public:
        using ContainerType = std::multimap<std::string, std::string, std::less<>>;

    private:
        ContainerType m_params; ///< Decoded parameters.

    public:
        /**
         * @brief Return the first value of a key, converted to T.
         * @tparam T Arithmetic type, bool ("1"/"true"/"0"/"false"), std::string, or std::string_view.
         * @param key Parameter name.
         * @return Converted value; string_view results borrow this object.
         * @throws std::runtime_error If the key is missing or the value does not convert.
         */
        template <typename T = std::string_view>
        requires std::integral<T> ||
            std::floating_point<T> ||
            std::same_as<T, std::string> ||
            std::same_as<T, std::string_view>
        [[nodiscard]] auto At(std::string_view key) const -> T {
            return convert<T>(atRaw(key));
        }

        /**
         * @brief Return every value of a key, converted to T, in insertion order.
         * @tparam T Same choices as At().
         * @param key Parameter name.
         * @return Converted values; empty when the key is missing.
         * @throws std::runtime_error If a value does not convert.
         */
        template <typename T = std::string_view>
        requires std::integral<T> ||
            std::floating_point<T> ||
            std::same_as<T, std::string> ||
            std::same_as<T, std::string_view>
        [[nodiscard]] auto All(std::string_view key) const -> std::vector<T> {
            std::vector<T> out;
            auto const [first, last]{ m_params.equal_range(key) };
            for (auto it{ first }; it != last; ++it) {
                out.push_back(convert<T>(it->second));
            }
            return out;
        }

        /**
         * @brief Append a value under a key.
         * @tparam T Arithmetic type, bool, or anything convertible to std::string_view.
         * @param key Parameter name.
         * @param value Value to store; bool becomes "true"/"false".
         */
        template <typename T>
        requires std::integral<T> ||
            std::floating_point<T> ||
            std::convertible_to<T const&, std::string_view>
        auto Add(std::string_view key, T const& value) -> void {
            if constexpr (std::same_as<T, bool>) {
                m_params.emplace(std::string{ key }, value ? "true" : "false");
            } else if constexpr (std::convertible_to<T const&, std::string_view>) {
                m_params.emplace(std::string{ key }, std::string{ std::string_view{ value } });
            } else {
                m_params.emplace(std::string{ key }, std::to_string(value));
            }
        }

        /**
         * @brief Report whether a key is present.
         * @param key Parameter name.
         * @return True when at least one value exists.
         */
        [[nodiscard]] auto Has(std::string_view key) const -> bool {
            return m_params.contains(key);
        }

        /**
         * @brief Report whether no parameter is stored.
         * @return True when empty.
         */
        [[nodiscard]] auto Empty() const noexcept -> bool {
            return m_params.empty();
        }

        /**
         * @brief Borrow the underlying container.
         * @return Parameters ordered by key, then insertion.
         */
        [[nodiscard]] auto Params() const noexcept -> ContainerType const& {
            return m_params;
        }

        /**
         * @brief Replace the contents by parsing "a=1&b=2" text.
         *
         * Keys and values are percent-decoded and '+' becomes a space (form-urlencoded rules).
         * A part without '=' is a key with an empty value; only the first '=' splits a part.
         *
         * @param content Encoded parameters without the leading '?'.
         * @return Always true; malformed escapes are kept verbatim.
         */
        auto Decode(std::string_view content) -> bool {
            m_params.clear();
            for (auto const item : detail::split(content, "&")) {
                if (item.empty()) {
                    continue;
                }
                auto const equals{ item.find('=') };
                auto const key{ item.substr(0, equals) };
                auto const value{ equals == std::string_view::npos ? std::string_view{} : item.substr(equals + 1) };
                m_params.emplace(decodeComponent(key), decodeComponent(value));
            }
            return true;
        }

        /**
         * @brief Encode as "a=1&b=2" text.
         * @return Percent-encoded parameters; spaces become '+'.
         */
        [[nodiscard]] auto Encoded() const -> std::string {
            std::string out;
            for (auto const& [key, value] : m_params) {
                if (!out.empty()) {
                    out += '&';
                }
                out += url::url_encode(key);
                out += '=';
                out += url::url_encode(value);
            }
            return out;
        }

    private:
        /**
         * @brief Return the first value of a key.
         * @param key Parameter name.
         * @return Borrowed value.
         * @throws std::runtime_error If the key is missing.
         */
        [[nodiscard]] auto atRaw(std::string_view key) const -> std::string_view {
            auto const it{ m_params.find(key) };
            if (it == m_params.end()) {
                throw std::runtime_error{ "Key not found: " + std::string{ key } };
            }
            return it->second;
        }

        /**
         * @brief Decode one form-urlencoded component.
         * @param component Encoded key or value.
         * @return Decoded text with '+' as space.
         */
        static auto decodeComponent(std::string_view component) -> std::string {
            std::string plus_decoded{ component };
            std::ranges::replace(plus_decoded, '+', ' ');
            return url::url_decode(plus_decoded);
        }

        /**
         * @brief Convert a stored value to T.
         * @tparam T Target type.
         * @param value Stored value.
         * @return Converted value.
         * @throws std::runtime_error If the conversion fails.
         */
        template <typename T>
        static auto convert(std::string_view value) -> T {
            if constexpr (std::same_as<T, std::string_view>) {
                return value;
            } else if constexpr (std::same_as<T, std::string>) {
                return std::string{ value };
            } else if constexpr (std::same_as<T, bool>) {
                if (value == "1" || value == "true") {
                    return true;
                }
                if (value == "0" || value == "false") {
                    return false;
                }
                throw std::runtime_error{ "query_param: invalid bool: " + std::string{ value } };
            } else {
                return detail::from_chars_strict<T>(value);
            }
        }
    };

} // namespace mhttp
