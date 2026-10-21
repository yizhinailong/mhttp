/**
 * @file response_impl.cppm
 * @brief Own immutable parsed client response metadata, trailers, endpoints, and buffered content.
 */
module mhttp:client.impl.response;

import std;
import :core.message;
import :core.endpoint;

namespace mhttp::impl {

    /**
     * @brief Retain a complete response independently of parser buffers, sockets, and the client lifetime.
     */
    struct ClientResponseImpl {
        detail::ResponseHead head{ .status = Status::Unknown }; ///< Owned parsed head; Unknown before parsing.
        Headers trailers; ///< Validated fields kept separate from ordinary response headers.
        std::string body; ///< Complete decoded buffered representation.
        Endpoint remote; ///< Owned peer snapshot if available.
        Endpoint local; ///< Owned local socket snapshot if available.
        bool keep_alive{}; ///< Reuse policy after complete response framing, false for EOF-delimited input.
    };

} // namespace mhttp::impl
