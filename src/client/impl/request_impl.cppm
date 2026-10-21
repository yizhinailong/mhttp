/**
 * @file request_impl.cppm
 * @brief Own outgoing client request data and validate mutations without transport side effects.
 */
module mhttp:client.impl.request;

import std;
import :core.message;
import :core.query;
import :utils.string;

namespace mhttp::impl {

    /**
     * @brief Own one outgoing request before its complete state transfers into a Send operation.
     */
    struct ClientRequestImpl {
        std::string method; ///< Case-sensitive method token, validated before transmission.
        std::string target; ///< Original request target, including query and optional local fragment.
        QueryParams query; ///< Additional parameters appended after the target's existing query.
        Headers headers; ///< Owned validated fields in insertion order.
        std::string body; ///< Owned buffered binary representation.

        /**
         * @brief Retain constructor arguments for validation by the sending boundary.
         * @param method_token Owned method, possibly invalid until Send validates it.
         * @param request_target Owned raw target.
         */
        ClientRequestImpl(std::string method_token, std::string request_target) :
            method{ std::move(method_token) }, target{ std::move(request_target) } {}

        /**
         * @brief Replace a method only after complete token validation and copying self-borrowed bytes.
         * @param value Borrowed case-sensitive method token.
         * @return Success or InvalidRequest, preserving the old method on rejection.
         */
        auto SetMethod(std::string_view value) -> Result<void> {
            if (!utils::is_field_name(value)) { return std::unexpected{ Error::InvalidRequest }; }
            method = std::string{ value };
            return {};
        }

        /**
         * @brief Replace binary content after validating its optional media type.
         * @param bytes Owned representation.
         * @param content_type Borrowed field value; empty removes Content-Type.
         * @return Success or InvalidHeaders, preserving all state on rejection.
         */
        auto SetContent(std::string bytes, std::string_view content_type) -> Result<void> {
            if (content_type.empty()) { headers.Erase("Content-Type"); }
            else {
                auto assigned = headers.Set("Content-Type", content_type);
                if (!assigned) { return assigned; }
            }
            body = std::move(bytes);
            return {};
        }
    };

} // namespace mhttp::impl
