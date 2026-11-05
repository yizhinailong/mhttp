/**
 * @file body_state.cppm
 * @brief Hold a materialized message body as one of the supported business types.
 */
export module mhttp:body_state;

import std;
import :config;
import :query_params;
import :form_data;

export namespace mhttp {

    /**
     * @brief Type of a materialized body, chosen from Content-Type when it was read.
     */
    enum class BodyType {
        None,        ///< Not read yet.
        Empty,       ///< Explicitly empty (no body on the wire).
        String,      ///< Text or any unrecognized content type.
        Json,        ///< application/json.
        QueryParams, ///< application/x-www-form-urlencoded.
        FormData,    ///< multipart/form-data.
    };

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Body not read yet.
     */
    struct NoneTag {};

    /**
     * @brief Explicitly empty body.
     */
    struct EmptyTag {};

    /**
     * @brief Materialized body result shared by every reader.
     */
    class BodyState {
    private:
        std::variant<NoneTag, EmptyTag, std::string, Json, QueryParams, FormData> m_state; ///< Current body.

    public:
        /**
         * @brief Report whether a body was materialized.
         * @return False only before reading.
         */
        [[nodiscard]] auto Has() const -> bool {
            return Type() != BodyType::None;
        }

        /**
         * @brief Map the active alternative to its BodyType.
         * @return Current body type.
         */
        [[nodiscard]] auto Type() const -> BodyType {
            return std::visit(
                [](auto const& value) -> BodyType {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, NoneTag>) {
                        return BodyType::None;
                    } else if constexpr (std::is_same_v<T, EmptyTag>) {
                        return BodyType::Empty;
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        return BodyType::String;
                    } else if constexpr (std::is_same_v<T, Json>) {
                        return BodyType::Json;
                    } else if constexpr (std::is_same_v<T, QueryParams>) {
                        return BodyType::QueryParams;
                    } else {
                        static_assert(std::is_same_v<T, FormData>);
                        return BodyType::FormData;
                    }
                },
                m_state
            );
        }

        /**
         * @brief Borrow the body as T.
         *
         * An explicitly empty body reads as an empty T: "no body" and "empty string" are the
         * same to callers. Unread bodies and other types are caller errors.
         *
         * @tparam T One of the stored types.
         * @return Borrowed value.
         * @throws std::bad_variant_access If the body is unread or holds another type.
         */
        template <typename T>
        [[nodiscard]] auto As() const -> T const& {
            if (Holds<EmptyTag>()) {
                static T const s_empty{};
                return s_empty;
            }
            return std::get<T>(m_state);
        }

        /**
         * @brief Report whether the active alternative is T.
         * @tparam T Alternative to test.
         * @return True when T is active.
         */
        template <typename T>
        [[nodiscard]] auto Holds() const -> bool {
            return std::holds_alternative<T>(m_state);
        }

        /**
         * @brief Replace the body.
         * @tparam T Alternative to store.
         * @param value Value to store.
         */
        template <typename T>
        auto Set(T value = {}) -> void {
            m_state = std::move(value);
        }

        /**
         * @brief Move the active value out; the alternative stays active in moved-from state.
         * @tparam T Active alternative.
         * @return Moved value.
         * @throws std::bad_variant_access If T is not active.
         */
        template <typename T>
        auto Take() -> T {
            return std::get<T>(std::move(m_state));
        }
    };

} // namespace mhttp::detail
