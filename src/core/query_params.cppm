/**
 * @file query_params.cppm
 * @brief Own ordered, case-sensitive query and URL-encoded form parameters.
 */
export module mhttp:core.query;

import std;
import :utils.string;
import :utils.percent;

export namespace mhttp {

    /**
     * @brief Own repeated query parameters in insertion order without sorting by key.
     * @note Views and iterators expire on mutation or destruction. Mutation needs external synchronization.
     */
    class QueryParams {
    private:
        std::vector<std::pair<std::string, std::string>> m_fields; ///< Owned parameters in wire order.

    public:
        using ConstIterator = decltype(m_fields)::const_iterator; ///< Read-only ordered parameter iterator.

        /**
         * @brief Decode a query string or URL-encoded form body into owned parameters.
         * @param query Borrowed bytes, optionally prefixed by ?; + decodes as SP.
         * @return Parsed collection; empty segments and raw whitespace-only names are ignored.
         * @note Split at the first = only. A nonempty key without = gets an empty value.
         *       Explicit empty names such as =value are supported. Malformed escapes remain literal.
         */
        [[nodiscard]] static auto Parse(std::string_view query) -> QueryParams {
            if (query.starts_with('?')) {
                query.remove_prefix(1);
            }
            return ParseForm(query);
        }

        /**
         * @brief Decode an owned URL-encoded form collection without treating an initial ? as a delimiter.
         * @param body Borrowed encoded form bytes; + decodes to SP, including in parameter names.
         * @return Independent ordered parameters; duplicate/empty names and binary values are preserved.
         * @note Empty segments and raw whitespace-only names are ignored. Malformed escapes remain literal,
         *       as in Parse; the input is split at & and the first = before percent decoding.
         */
        [[nodiscard]] static auto ParseForm(std::string_view body) -> QueryParams {
            QueryParams result;
            for (auto part : utils::split(body, '&')) {
                if (part.empty()) {
                    continue;
                }
                auto const separator = part.find('=');
                auto const name = part.substr(0, separator);
                if (!name.empty() && utils::trim_ows(name).empty()) {
                    continue;
                }
                auto const value = separator == std::string_view::npos ? std::string_view{} : part.substr(separator + 1);
                result.Add(UrlDecode(name, UrlEncoding::QueryComponent),
                    UrlDecode(value, UrlEncoding::QueryComponent));
            }
            return result;
        }

        /**
         * @brief Append a copied parameter, including repeated or empty names.
         * @param name Borrowed case-sensitive key.
         * @param value Borrowed binary value.
         */
        auto Add(std::string_view name, std::string_view value) -> void {
            auto field = std::pair{ std::string{ name }, std::string{ value } };
            m_fields.push_back(std::move(field));
        }

        /**
         * @brief Remove exact-name matches and append an owned replacement.
         * @param name Borrowed case-sensitive key; may refer to this collection.
         * @param value Borrowed replacement value; may refer to this collection.
         */
        auto Set(std::string_view name, std::string_view value) -> void {
            auto field = std::pair{ std::string{ name }, std::string{ value } };
            Erase(field.first);
            m_fields.push_back(std::move(field));
        }

        /**
         * @brief Erase all exact-name matches while preserving the other parameters' order.
         * @param name Borrowed key; may refer to this collection.
         * @return Removed parameter count.
         */
        auto Erase(std::string_view name) -> std::size_t {
            std::string const key{ name };
            return std::erase_if(m_fields, [&key](auto const& field) { return field.first == key; });
        }

        /**
         * @brief Borrow the first exact-name match or a fallback.
         * @param name Case-sensitive lookup key.
         * @param fallback Borrowed missing-value default.
         * @return View into collection storage or fallback, requiring the referenced storage to live on.
         */
        [[nodiscard]] auto Get(
            std::string_view name,
            std::string_view fallback = {}
        ) const noexcept -> std::string_view {
            for (auto const& [key, value] : m_fields) {
                if (key == name) {
                    return value;
                }
            }
            return fallback;
        }

        /**
         * @brief Strictly convert a parameter without exceptions for malformed values.
         * @tparam T Numeric type accepted by from_chars, or bool (true, false, 1, 0 only).
         * @param name Borrowed case-sensitive key.
         * @return Converted first value, or nullopt for missing, malformed, overflowing, or nonfinite input.
         */
        template <typename T>
            requires std::same_as<T, bool> || ((std::integral<T> || std::floating_point<T>)
                && requires(char const* begin, char const* end, T& value) {
                    std::from_chars(begin, end, value);
                })
        [[nodiscard]] auto Get(std::string_view name) const noexcept -> std::optional<T> {
            auto const text = Get(name);
            if constexpr (std::same_as<T, bool>) {
                if (text == "true" || text == "1") { return true; }
                if (text == "false" || text == "0") { return false; }
                return std::nullopt;
            } else if constexpr (std::integral<T>) {
                return utils::parse_integer<T>(text);
            } else {
                if (text.empty()) {
                    return std::nullopt;
                }
                T value{};
                auto const end = text.data() + text.size();
                auto const parsed = std::from_chars(text.data(), end, value);
                if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value)) {
                    return std::nullopt;
                }
                return value;
            }
        }

        /**
         * @brief Test for a parameter, including one with an empty value.
         * @param name Borrowed case-sensitive key.
         * @return Whether any key matches.
         */
        [[nodiscard]] auto Has(std::string_view name) const noexcept -> bool {
            return std::ranges::any_of(m_fields, [name](auto const& field) { return field.first == name; });
        }

        /**
         * @brief Count all exact-name matches.
         * @param name Borrowed case-sensitive key.
         * @return Matching parameter count.
         */
        [[nodiscard]] auto Count(std::string_view name) const noexcept -> std::size_t {
            return static_cast<std::size_t>(std::ranges::count_if(m_fields,
                [name](auto const& field) { return field.first == name; }));
        }

        /**
         * @brief Borrow matching values in insertion order.
         * @param name Borrowed case-sensitive key.
         * @return Views valid until this collection changes or is destroyed.
         */
        [[nodiscard]] auto Values(std::string_view name) const -> std::vector<std::string_view> {
            std::vector<std::string_view> result;
            for (auto const& [key, value] : m_fields) {
                if (key == name) {
                    result.push_back(value);
                }
            }
            return result;
        }

        /**
         * @brief Encode parameters in insertion order using query-component rules.
         * @return Owned query or form bytes, without a leading ?.
         */
        [[nodiscard]] auto Encode() const -> std::string {
            std::string result;
            for (std::size_t index{}; index < m_fields.size(); ++index) {
                if (index != 0) { result.push_back('&'); }
                auto const& [key, value] = m_fields[index];
                result += UrlEncode(key, UrlEncoding::QueryComponent);
                result.push_back('=');
                result += UrlEncode(value, UrlEncoding::QueryComponent);
            }
            return result;
        }

        /**
         * @brief Append encoded parameters to an existing target, before any fragment.
         * @param target Borrowed path, optionally containing query and fragment components.
         * @return Owned target with parameters appended without changing existing query order.
         */
        [[nodiscard]] auto AppendTo(std::string_view target) const -> std::string {
            if (Empty()) { return std::string{ target }; }
            auto const fragment = target.find('#');
            auto const prefix = target.substr(0, fragment);
            std::string result{ prefix };
            if (!prefix.contains('?')) {
                result.push_back('?');
            } else if (!prefix.ends_with('?') && !prefix.ends_with('&')) {
                result.push_back('&');
            }
            result += Encode();
            if (fragment != std::string_view::npos) { result += target.substr(fragment); }
            return result;
        }

        /**
         * @brief Count all parameters, including repeated keys.
         * @return Stored parameter count.
         */
        [[nodiscard]] auto Size() const noexcept -> std::size_t { return m_fields.size(); }

        /**
         * @brief Test whether the collection has no parameters.
         * @return Whether Size is zero.
         */
        [[nodiscard]] auto Empty() const noexcept -> bool { return m_fields.empty(); }

        /**
         * @brief Remove all parameters and invalidate borrowed views.
         */
        auto Clear() noexcept -> void { m_fields.clear(); }

        /**
         * @brief Begin read-only iteration in insertion order.
         * @return Iterator valid until mutation or destruction.
         */
        [[nodiscard]] auto begin() const noexcept -> ConstIterator { return m_fields.begin(); }

        /**
         * @brief Obtain the end of read-only iteration.
         * @return End iterator valid until mutation or destruction.
         */
        [[nodiscard]] auto end() const noexcept -> ConstIterator { return m_fields.end(); }
    };

} // namespace mhttp
