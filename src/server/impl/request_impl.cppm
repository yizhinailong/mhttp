/**
 * @file request_impl.cppm
 * @brief Own parsed server request metadata, decoded accessors, buffered content, and request context.
 */
module mhttp:server.impl.request;

import std;
import :core.message;
import :core.query;
import :core.endpoint;
import :utils.percent;
import :server.request_data;

namespace mhttp::impl {

    /**
     * @brief Keep one request's storage independent of parser buffers and its connection lifetime.
     * @note Session initialization and handler access are serialized; only RequestData has internal locking.
     */
    struct ServerRequestImpl {
        detail::RequestHead head; ///< Owned validated method, target, version, and fields.
        std::string path; ///< Decoded path for application inspection, not for splitting routing segments.
        QueryParams query; ///< Ordered decoded query parameters.
        QueryParams path_params; ///< Owned route captures populated before middleware and handler execution.
        std::string body; ///< Bounded decoded content, populated after the parser finishes.
        Headers trailers; ///< Validated trailers, kept separate from ordinary fields.
        Endpoint remote; ///< Owned peer endpoint.
        Endpoint local; ///< Owned local endpoint.
        server::RequestData data; ///< Thread-safe per-request context.
        bool keep_alive; ///< HTTP persistence semantics from the original request metadata.

        /**
         * @brief Own validated parser metadata and derive independent path and query storage.
         * @param source Owned normalized origin-form request head, or OPTIONS asterisk.
         * @param remote_endpoint Owned peer endpoint, optionally empty in standalone tests.
         * @param local_endpoint Owned local endpoint, optionally empty in standalone tests.
         */
        ServerRequestImpl(
            detail::RequestHead source,
            Endpoint remote_endpoint,
            Endpoint local_endpoint
        ) : head{ std::move(source) }, remote{ std::move(remote_endpoint) }, local{ std::move(local_endpoint) },
            keep_alive{ detail::allows_keep_alive(head.version, head.headers) } {
            path = UrlDecode(EncodedPath(), UrlEncoding::PathSegment);
            auto const separator = head.target.find('?');
            if (separator != std::string::npos) {
                query = QueryParams::Parse(std::string_view{ head.target }.substr(separator + 1));
            }
        }

        /**
         * @brief Borrow the encoded path while excluding the original query string.
         * @return View into head.target; routing must split this view before decoding it.
         */
        [[nodiscard]] auto EncodedPath() const noexcept -> std::string_view {
            return std::string_view{ head.target }.substr(0, head.target.find('?'));
        }
    };

} // namespace mhttp::impl
