/**
 * @file http_message.cppm
 * @brief Define HTTP wire metadata, bounded parsing events, and shared framing rules.
 */
export module mhttp:core.message;

import std;
import :utils.string;
import :core.url;
export import :core.error;
export import :core.headers;
export import :core.method;
export import :core.status;

export namespace mhttp {

    /**
     * @brief Identify supported HTTP wire versions.
     */
    enum class Version {
        Http10, ///< HTTP/1.0.
        Http11, ///< HTTP/1.1.
    };

    /**
     * @brief Obtain the exact HTTP version token.
     * @param version Supported version.
     * @return Static wire spelling, or an empty view for an invalid value.
     */
    [[nodiscard]] constexpr auto ToString(Version version) noexcept -> std::string_view {
        switch (version) {
        case Version::Http10: return "HTTP/1.0";
        case Version::Http11: return "HTTP/1.1";
        }
        return {};
    }

} // namespace mhttp

export namespace mhttp::detail {

    /**
     * @brief Own request-line metadata and ordered fields without owning the body.
     * @internal
     */
    struct RequestHead {
        std::string method{ "GET" }; ///< Case-sensitive method token.
        std::string target{ "/" }; ///< Encoded origin-form target, or OPTIONS *.
        Version version{ Version::Http11 }; ///< Parsed wire version.
        Headers headers; ///< Owned fields in wire order.
        std::optional<std::string> absolute_authority; ///< Authority supplied by an absolute-form target.
    };

    /**
     * @brief Own response-line metadata and ordered fields without owning the body.
     * @internal
     */
    struct ResponseHead {
        Version version{ Version::Http11 }; ///< Parsed wire version.
        Status status{ Status::Ok }; ///< Three-digit status, including extension values.
        std::string reason; ///< Reason bytes; may be empty.
        Headers headers; ///< Owned fields in wire order.
    };

    /**
     * @brief Bound parser memory and work before accepting untrusted framing information.
     * @internal
     */
    struct ParserLimits {
        std::size_t uri_bytes{ 8 * 1024 }; ///< Encoded request target bytes, excluding method and version.
        std::size_t start_line_bytes{ 64 * 1024 }; ///< Start-line bytes, excluding CRLF.
        std::size_t header_bytes{ 64 * 1024 }; ///< Fields and trailers together, including their CRLFs and terminators.
        std::size_t header_line_bytes{ 64 * 1024 }; ///< Individual field bytes, excluding CRLF.
        std::size_t header_fields{ 100 }; ///< Fields and trailers together, including discarded trailers.
        std::uint64_t body_bytes{ 8 * 1024 * 1024 }; ///< Decoded body byte limit.
        std::size_t chunk_line_bytes{ 8 * 1024 }; ///< Chunk-size line, including extensions, excluding CRLF.
        std::size_t interim_responses{ 8 }; ///< Maximum skipped informational responses.
    };

    /**
     * @brief Describe a fatal parse failure and the server status to emit before closing.
     * @internal
     */
    struct ProtocolFailure {
        Error error{ Error::ProtocolError }; ///< Transport-independent failure classification.
        Status status{ Status::BadRequest }; ///< Suggested server response; every failure requires closure.
    };

    /**
     * @brief Represent a protocol result whose error includes a response status.
     * @tparam T Successful result type.
     * @internal
     */
    template <typename T>
    using ProtocolResult = std::expected<T, ProtocolFailure>;

    /**
     * @brief Identify the next externally observable parser event.
     * @internal
     */
    enum class ParseEvent {
        NeedMore, ///< All supplied bytes were consumed; supply more or signal EOF.
        HeadersComplete, ///< Final headers are available before body processing starts.
        Body, ///< A nonempty borrowed body span is available.
        Complete, ///< One message ended; leave all unconsumed bytes for its successor.
        Failed, ///< Fatal error; discard the connection instead of parsing a successor.
    };

    /**
     * @brief Return consumption and one event without copying body or successor bytes.
     * @internal
     */
    struct ParseStep {
        ParseEvent event{ ParseEvent::NeedMore }; ///< Current event.
        std::size_t consumed{}; ///< Prefix consumed from this Feed call only.
        std::span<char const> data; ///< Body-only view, valid while the caller keeps its input unchanged.
        std::optional<ProtocolFailure> failure; ///< Present exactly when event is Failed.
    };

    /**
     * @brief Identify how a message body ends.
     * @internal
     */
    enum class BodyKind {
        None, ///< No wire body.
        ContentLength, ///< Exact byte count.
        Chunked, ///< Chunked framing.
        UntilEof, ///< Response body ends only when the connection closes.
    };

    /**
     * @brief Summarize validated framing and connection semantics.
     * @internal
     */
    struct MessageFraming {
        BodyKind kind{ BodyKind::None }; ///< Body framing mode.
        std::uint64_t content_length{}; ///< Content-Length, if present, without allocating body storage.
        bool keep_alive{}; ///< Whether HTTP semantics permit connection reuse.
        bool expect_continue{}; ///< HTTP/1.1 request explicitly expects 100-continue.
    };

    /**
     * @brief Match comma-separated tokens across all lines of a header.
     * @param headers Borrowed field collection.
     * @param name Case-insensitive field name.
     * @param token Case-insensitive complete token.
     * @return Whether any trimmed list element matches exactly.
     * @internal
     */
    [[nodiscard]] auto has_header_token(
        Headers const& headers,
        std::string_view name,
        std::string_view token
    ) noexcept -> bool {
        for (auto const& [key, text] : headers) {
            if (!utils::iequals(key, name)) { continue; }
            std::string_view remaining{ text };
            for (;;) {
                auto const comma = remaining.find(',');
                if (utils::iequals(utils::trim_ows(remaining.substr(0, comma)), token)) { return true; }
                if (comma == std::string_view::npos) { break; }
                remaining.remove_prefix(comma + 1);
            }
        }
        return false;
    }

    /**
     * @brief Apply HTTP version defaults and whole-token Connection overrides.
     * @param version Wire version.
     * @param headers Borrowed fields.
     * @return Whether persistence is allowed; close always overrides keep-alive.
     * @internal
     */
    [[nodiscard]] auto allows_keep_alive(Version version, Headers const& headers) noexcept -> bool {
        return !has_header_token(headers, "Connection", "close")
            && (version == Version::Http11 || has_header_token(headers, "Connection", "keep-alive"));
    }

    /**
     * @brief Validate Host authority syntax without permitting userinfo or a path suffix.
     * @param value Trimmed Host field.
     * @return Whether the field is a valid authority for an HTTP URL.
     * @internal
     */
    [[nodiscard]] auto is_valid_host(std::string_view value) -> bool {
        return !value.empty() && value.find_first_of("@/\\?# \t") == std::string_view::npos
            && Url::Parse("http://" + std::string{ value }).has_value();
    }

    /**
     * @brief Reject trailers that affect framing, routing, authentication, or content processing.
     * @param name Borrowed trailer name.
     * @param headers Initial headers, including any connection-specific field names.
     * @return Whether the validated trailer must be discarded instead of exposed to consumers.
     * @internal
     */
    [[nodiscard]] auto is_forbidden_trailer(
        std::string_view name,
        Headers const& headers
    ) noexcept -> bool {
        constexpr std::string_view FORBIDDEN[]{
            "Content-Length", "Transfer-Encoding", "Host", "Connection", "Trailer", "TE", "Expect",
            "Authorization", "Proxy-Authorization", "Cookie", "Set-Cookie", "Content-Type",
            "Content-Encoding", "Content-Range", "Range", "Cache-Control", "Max-Forwards"
        };
        return std::ranges::any_of(FORBIDDEN, [name](auto field) { return utils::iequals(name, field); })
            || has_header_token(headers, "Connection", name);
    }

    /**
     * @brief Determine framing after fields have passed lexical validation.
     * @param version Wire version.
     * @param headers Borrowed validated headers.
     * @param limits Configured body limit.
     * @param response Whether this is a response; requests without framing have no body.
     * @param bodyless Whether request-method or response-status semantics forbid a response body.
     * @return Framing or a fatal protocol failure; this routine never reserves body memory.
     * @internal
     */
    [[nodiscard]] auto analyze_framing(
        Version version,
        Headers const& headers,
        ParserLimits const& limits,
        bool response = false,
        bool bodyless = false
    ) -> ProtocolResult<MessageFraming> {
        auto const bad = std::unexpected{ ProtocolFailure{} };
        if (!response) {
            auto const hosts = headers.Count("Host");
            if (hosts > 1 || (version == Version::Http11 && hosts == 0)
                || (hosts == 1 && !is_valid_host(headers.Get("Host")))) { return bad; }
        }
        bool const has_transfer = headers.Has("Transfer-Encoding");
        if (has_transfer && headers.Has("Content-Length")) { return bad; }
        std::optional<std::uint64_t> length;
        bool chunked{};
        bool unknown_transfer{};
        for (auto const& [name, value] : headers) {
            if (utils::iequals(name, "Content-Length")) {
                if (value.empty() || !std::ranges::all_of(value, [](char byte) {
                    return byte >= '0' && byte <= '9';
                })) { return bad; }
                auto const parsed = utils::parse_integer<std::uint64_t>(value);
                if (!parsed || (length && *length != *parsed)) { return bad; }
                length = *parsed;
            } else if (utils::iequals(name, "Transfer-Encoding")) {
                if (version != Version::Http11) { return bad; }
                for (auto token : utils::split(value, ',')) {
                    token = utils::trim_ows(token);
                    if (!utils::is_field_name(token) || chunked) { return bad; }
                    if (utils::iequals(token, "chunked")) { chunked = true; }
                    else { unknown_transfer = true; }
                }
                if (value.empty()) { return bad; }
            }
        }
        if (unknown_transfer || (has_transfer && !chunked)) {
            return std::unexpected{ ProtocolFailure{ Error::ProtocolError,
                response ? Status::BadRequest : Status::NotImplemented } };
        }
        MessageFraming result;
        result.keep_alive = allows_keep_alive(version, headers);
        result.content_length = length.value_or(0);
        if (!response && version == Version::Http11) {
            for (auto const& [name, value] : headers) {
                if (!utils::iequals(name, "Expect")) { continue; }
                if (value.empty()) { return std::unexpected{ ProtocolFailure{ Error::ProtocolError, Status::ExpectationFailed } }; }
                for (auto token : utils::split(value, ',')) {
                    if (!utils::iequals(utils::trim_ows(token), "100-continue")) {
                        return std::unexpected{ ProtocolFailure{ Error::ProtocolError, Status::ExpectationFailed } };
                    }
                    result.expect_continue = true;
                }
            }
        }
        if (bodyless) { return result; }
        if (chunked) { result.kind = BodyKind::Chunked; }
        else if (length) {
            if (*length > limits.body_bytes) {
                return std::unexpected{ ProtocolFailure{ Error::BodyTooLarge, Status::ContentTooLarge } };
            }
            result.kind = *length == 0 ? BodyKind::None : BodyKind::ContentLength;
        } else if (response) {
            result.kind = BodyKind::UntilEof;
            result.keep_alive = false;
        }
        return result;
    }

} // namespace mhttp::detail
