/**
 * @file response_impl.cppm
 * @brief Own buffered server response state and validate content changes before publication.
 */
module mhttp:server.impl.response;

import std;
import :core.message;

namespace mhttp::impl {

    /**
     * @brief Own one buffered response, accessed only by its request's serialized handler chain.
     */
    struct ServerResponseImpl {
        mhttp::Status status{ mhttp::Status::Ok }; ///< Current three-digit status.
        mhttp::Headers headers; ///< Validated ordered response fields.
        std::string body; ///< Owned binary representation, independent of wire body suppression.

        /**
         * @brief Set a three-digit response status without changing content.
         * @param value Known or extension status in [100, 999].
         * @return Success or InvalidRequest, leaving state unchanged on invalid input.
         */
        auto SetStatus(mhttp::Status value) -> Result<void> {
            auto const code = static_cast<unsigned>(value);
            if (code < 100 || code > 999) { return std::unexpected{ Error::InvalidRequest }; }
            status = value;
            return {};
        }

        /**
         * @brief Replace content while preserving the current status and unrelated fields.
         * @param bytes Owned binary body.
         * @param content_type Borrowed validated field value; empty removes Content-Type.
         * @return Success or InvalidHeaders before any content changes.
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

        /**
         * @brief Set an empty representation and remove its content type.
         * @param value Three-digit status.
         * @return Success or InvalidRequest before any content changes.
         */
        auto SetEmpty(mhttp::Status value) -> Result<void> {
            auto assigned = SetStatus(value);
            if (!assigned) { return assigned; }
            headers.Erase("Content-Type");
            body.clear();
            return {};
        }

        /**
         * @brief Set a plain-text default error body containing only the code and static reason phrase.
         * @param value Client or server error status.
         * @return Success or InvalidRequest for statuses outside [400, 599].
         */
        auto SetError(mhttp::Status value) -> Result<void> {
            if (!IsClientError(value) && !IsServerError(value)) { return std::unexpected{ Error::InvalidRequest }; }
            auto bytes = std::format("{} {}\n", static_cast<unsigned>(value), ReasonPhrase(value));
            static_cast<void>(headers.Set("Content-Type", "text/plain; charset=utf-8"));
            body = std::move(bytes);
            status = value;
            return {};
        }

        /**
         * @brief Set a Location field and an empty redirect representation.
         * @param url Nonempty relative or absolute reference, copied before current state changes.
         * @param value Redirect status other than 304.
         * @return Success, InvalidUrl for an empty reference, InvalidRequest for an invalid status,
         *         or InvalidHeaders for an unsafe field value, preserving state on rejection.
         */
        auto SetRedirect(std::string_view url, mhttp::Status value) -> Result<void> {
            if (!IsRedirect(value) || value == mhttp::Status::NotModified) {
                return std::unexpected{ Error::InvalidRequest };
            }
            if (url.empty()) { return std::unexpected{ Error::InvalidUrl }; }
            auto assigned = headers.Set("Location", url);
            if (!assigned) { return assigned; }
            headers.Erase("Content-Type");
            body.clear();
            status = value;
            return {};
        }
    };

} // namespace mhttp::impl
