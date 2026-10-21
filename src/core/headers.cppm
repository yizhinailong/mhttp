/**
 * @file headers.cppm
 * @brief Store validated HTTP header fields in wire order with case-insensitive lookup.
 */
export module mhttp:core.headers;

import std;
import :utils.string;
export import :core.error;

export namespace mhttp {

    /**
     * @brief Own ordered HTTP fields, preserving original name spelling and repeated values.
     * @note Iteration is read-only so all writes pass validation. Views and iterators must not be
     *       retained across mutation or destruction. Concurrent mutation requires external synchronization.
     */
    class Headers {
    private:
        std::vector<std::pair<std::string, std::string>> m_fields; ///< Owned fields in insertion order.

    public:
        using ConstIterator = decltype(m_fields)::const_iterator; ///< Iterator over immutable name/value pairs.

        /**
         * @brief Append a validated field, preserving any existing values with the same name.
         * @param name Borrowed nonempty token; copied before storage changes.
         * @param value Borrowed field value with no surrounding OWS; copied before storage changes.
         * @return Success or InvalidHeaders, leaving the collection unchanged on rejection.
         */
        [[nodiscard]] auto Add(std::string_view name, std::string_view value) -> Result<void> {
            if (!utils::is_field_name(name) || !utils::is_field_value(value)) {
                return std::unexpected{ Error::InvalidHeaders };
            }
            auto field = std::pair{ std::string{ name }, std::string{ value } };
            m_fields.push_back(std::move(field));
            return {};
        }

        /**
         * @brief Remove all case-insensitive matches, then append a validated replacement.
         * @param name Borrowed nonempty token; may refer to an existing field in this collection.
         * @param value Borrowed validated value; may refer to an existing field in this collection.
         * @return Success or InvalidHeaders, preserving the collection on rejection.
         */
        [[nodiscard]] auto Set(std::string_view name, std::string_view value) -> Result<void> {
            if (!utils::is_field_name(name) || !utils::is_field_value(value)) {
                return std::unexpected{ Error::InvalidHeaders };
            }
            auto field = std::pair{ std::string{ name }, std::string{ value } };
            Erase(field.first);
            m_fields.push_back(std::move(field));
            return {};
        }

        /**
         * @brief Remove all fields with a matching name, preserving the remaining order.
         * @param name Borrowed case-insensitive name; may refer to this collection.
         * @return Number of removed fields.
         */
        auto Erase(std::string_view name) -> std::size_t {
            // Own the lookup key before vector compaction can move its source field.
            std::string const key{ name };
            return std::erase_if(m_fields, [&key](auto const& field) {
                return utils::iequals(field.first, key);
            });
        }

        /**
         * @brief Find the first value for a case-insensitive field name.
         * @param name Borrowed field name.
         * @param fallback Borrowed default returned when no field matches.
         * @return View into this collection or fallback; the referenced storage must remain valid.
         */
        [[nodiscard]] auto Get(
            std::string_view name,
            std::string_view fallback = {}
        ) const noexcept -> std::string_view {
            for (auto const& [key, value] : m_fields) {
                if (utils::iequals(key, name)) {
                    return value;
                }
            }
            return fallback;
        }

        /**
         * @brief Test for a case-insensitive field name, including fields with empty values.
         * @param name Borrowed field name.
         * @return Whether any field matches.
         */
        [[nodiscard]] auto Has(std::string_view name) const noexcept -> bool {
            return std::ranges::any_of(m_fields, [name](auto const& field) {
                return utils::iequals(field.first, name);
            });
        }

        /**
         * @brief Count all case-insensitive matches.
         * @param name Borrowed field name.
         * @return Number of matching fields.
         */
        [[nodiscard]] auto Count(std::string_view name) const noexcept -> std::size_t {
            return static_cast<std::size_t>(std::ranges::count_if(m_fields, [name](auto const& field) {
                return utils::iequals(field.first, name);
            }));
        }

        /**
         * @brief Borrow all values for one field name in insertion order.
         * @param name Borrowed case-insensitive name.
         * @return Views valid until this collection is mutated or destroyed.
         */
        [[nodiscard]] auto Values(std::string_view name) const -> std::vector<std::string_view> {
            std::vector<std::string_view> result;
            for (auto const& [key, value] : m_fields) {
                if (utils::iequals(key, name)) {
                    result.push_back(value);
                }
            }
            return result;
        }

        /**
         * @brief Count fields, including repeated names.
         * @return Number of stored fields.
         */
        [[nodiscard]] auto Size() const noexcept -> std::size_t { return m_fields.size(); }

        /**
         * @brief Test whether the collection has no fields.
         * @return Whether Size is zero.
         */
        [[nodiscard]] auto Empty() const noexcept -> bool { return m_fields.empty(); }

        /**
         * @brief Remove all fields and invalidate borrowed views.
         */
        auto Clear() noexcept -> void { m_fields.clear(); }

        /**
         * @brief Begin read-only iteration in wire order.
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

export namespace mhttp::header {

    inline constexpr std::string_view HOST{ "Host" }; ///< Request authority.
    inline constexpr std::string_view CONTENT_TYPE{ "Content-Type" }; ///< Body media type.
    inline constexpr std::string_view CONTENT_LENGTH{ "Content-Length" }; ///< Body length in bytes.
    inline constexpr std::string_view TRANSFER_ENCODING{ "Transfer-Encoding" }; ///< Message transfer coding.
    inline constexpr std::string_view CONNECTION{ "Connection" }; ///< Connection options.
    inline constexpr std::string_view DATE{ "Date" }; ///< Response timestamp.
    inline constexpr std::string_view LOCATION{ "Location" }; ///< Redirect reference.
    inline constexpr std::string_view AUTHORIZATION{ "Authorization" }; ///< Request credentials.
    inline constexpr std::string_view ALLOW{ "Allow" }; ///< Supported request methods.
    inline constexpr std::string_view EXPECT{ "Expect" }; ///< Request expectations.
    inline constexpr std::string_view TRAILER{ "Trailer" }; ///< Announced trailer fields.
    inline constexpr std::string_view USER_AGENT{ "User-Agent" }; ///< Client identification.

} // namespace mhttp::header
