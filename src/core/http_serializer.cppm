/**
 * @file http_serializer.cppm
 * @brief Validate and serialize HTTP heads with authoritative body framing.
 */
export module mhttp:core.serializer;

import std;
import :utils.string;
export import :core.message;

export namespace mhttp::detail {

    /**
     * @brief Describe the actual body and connection policy before generating a head.
     * @note HTTP/1.0 ignores chunked and uses body_bytes unless close_delimited is selected.
     *       Unknown-length responses may explicitly select close delimitation; requests may not.
     * @internal
     */
    struct SerializeOptions {
        std::uint64_t body_bytes{}; ///< Actual buffered size, including the representation for HEAD.
        bool chunked{}; ///< Prefer chunked HTTP/1.1 framing over a fixed size.
        bool keep_alive{ true }; ///< Permit persistence if version and Connection fields allow it.
        bool close_delimited{}; ///< Response-only unknown length; omit size/coding and force closure.
    };

    /**
     * @brief Own a complete validated head without borrowing the body.
     * @note Send bytes first, then send the body only when send_body is true. For Chunked, encode
     *       each body segment and the terminator separately. A HEAD response advertises the same
     *       framing as GET while send_body is false, including for unknown-length chunked bodies.
     * @internal
     */
    struct SerializedHead {
        std::string bytes; ///< Complete start line, fields, and terminating CRLF.
        BodyKind body_kind{ BodyKind::None }; ///< Advertised framing, independent of HEAD suppression.
        bool send_body{}; ///< Whether wire body bytes are permitted.
        bool keep_alive{}; ///< Final connection persistence decision.
    };

    /**
     * @brief Encode bytes that could delimit or invalidate a request line without changing escapes.
     * @param target Borrowed origin-form target or OPTIONS asterisk.
     * @return Owned target preserving existing percent escapes, query delimiters, and UTF-8 bytes.
     * @internal
     */
    [[nodiscard]] auto encode_request_target(std::string_view target) -> std::string {
        constexpr char HEX[]{ "0123456789ABCDEF" };
        std::string result;
        for (unsigned char byte : target) {
            if (byte <= 0x20 || byte == 0x7f) {
                result += '%';
                result += HEX[byte >> 4];
                result += HEX[byte & 0xf];
            } else { result += static_cast<char>(byte); }
        }
        return result;
    }

    /**
     * @brief Validate request Host semantics after Headers has enforced lexical validity.
     * @param headers Borrowed fields; callers must handle errors from rejected field mutations.
     * @param version Valid wire version.
     * @return Success or InvalidHeaders for missing, duplicate, or malformed Host.
     * @internal
     */
    [[nodiscard]] auto validate_outgoing_host(
        Headers const& headers,
        Version version
    ) -> Result<void> {
        auto const hosts = headers.Count("Host");
        if (hosts > 1 || (version == Version::Http11 && hosts == 0)
            || (hosts == 1 && !is_valid_host(headers.Get("Host")))) {
            return std::unexpected{ Error::InvalidHeaders };
        }
        return {};
    }

    /**
     * @brief Append ordered fields with framing recomputed from the actual body.
     * @param result Owned start line and final framing metadata to extend.
     * @param headers Borrowed validated caller fields, never modified.
     * @param version Wire version.
     * @param options Actual body and persistence policy.
     * @param bodyless Whether status semantics prohibit all body framing.
     * @internal
     */
    auto append_outgoing_headers(
        SerializedHead& result,
        Headers const& headers,
        Version version,
        SerializeOptions const& options,
        bool bodyless
    ) -> void {
        bool const chunked = !bodyless && version == Version::Http11 && options.chunked;
        bool const until_eof = !bodyless && options.close_delimited;
        result.keep_alive = !until_eof && options.keep_alive && allows_keep_alive(version, headers);
        result.body_kind = bodyless ? BodyKind::None : until_eof ? BodyKind::UntilEof
            : chunked ? BodyKind::Chunked : BodyKind::ContentLength;
        for (auto const& [name, value] : headers) {
            if (utils::iequals(name, "Content-Length") || utils::iequals(name, "Transfer-Encoding")
                || (utils::iequals(name, "Trailer") && !chunked)) { continue; }
            result.bytes += name;
            result.bytes += ": ";
            result.bytes += value;
            result.bytes += "\r\n";
        }
        if (!bodyless && !until_eof) {
            result.bytes += chunked ? "Transfer-Encoding: chunked\r\n"
                : std::format("Content-Length: {}\r\n", options.body_bytes);
        }
        if (!result.keep_alive && !has_header_token(headers, "Connection", "close")) {
            result.bytes += "Connection: close\r\n";
        } else if (result.keep_alive && version == Version::Http10
            && !has_header_token(headers, "Connection", "keep-alive")) {
            result.bytes += "Connection: keep-alive\r\n";
        }
        result.bytes += "\r\n";
    }

    /**
     * @brief Generate a validated request head with no transport side effects.
     * @param head Borrowed method, origin-form target, version, and fields; HTTP/1.1 requires Host.
     * @param options Actual body length or chunked preference and connection policy.
     * @return Owned head or an error before any bytes can be sent. Control bytes and spaces in
     *         target are percent encoded; fragments, non-origin targets, and invalid methods fail.
     * @internal
     */
    [[nodiscard]] auto serialize_request_head(
        RequestHead const& head,
        SerializeOptions const& options = {}
    ) -> Result<SerializedHead> {
        auto const version = ToString(head.version);
        if (options.close_delimited || version.empty() || !utils::is_field_name(head.method) || head.target.empty()
            || head.target.find('#') != std::string::npos
            || (head.target == "*" ? head.method != "OPTIONS" : !head.target.starts_with('/'))) {
            return std::unexpected{ Error::InvalidRequest };
        }
        auto const valid = validate_outgoing_host(head.headers, head.version);
        if (!valid) { return std::unexpected{ valid.error() }; }
        SerializedHead result;
        result.send_body = true;
        result.bytes = std::format("{} {} {}\r\n", head.method, encode_request_target(head.target), version);
        append_outgoing_headers(result, head.headers, head.version, options, false);
        return result;
    }

    /**
     * @brief Generate a response head with status and originating-method body rules.
     * @param head Borrowed response metadata; an empty reason selects the standard phrase.
     * @param response_to Request method; HEAD suppresses body without changing representation fields.
     * @param options Actual body and persistence policy; framing fields supplied in head are replaced.
     * @return Owned head or validation error. Informational, 204, and 304 statuses omit framing/body.
     * @internal
     */
    [[nodiscard]] auto serialize_response_head(
        ResponseHead const& head,
        Method response_to = Method::Get,
        SerializeOptions const& options = {}
    ) -> Result<SerializedHead> {
        auto const version = ToString(head.version);
        auto const status = static_cast<unsigned>(head.status);
        if (version.empty() || status < 100 || status > 999 || (options.chunked && options.close_delimited)) {
            return std::unexpected{ Error::InvalidRequest };
        }
        if (!std::ranges::all_of(head.reason, [](unsigned char byte) {
            return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
        })) { return std::unexpected{ Error::InvalidHeaders }; }
        bool const bodyless = IsInformational(head.status) || head.status == Status::NoContent
            || head.status == Status::NotModified;
        SerializedHead result;
        result.send_body = !bodyless && response_to != Method::Head;
        auto const reason = head.reason.empty() ? ReasonPhrase(head.status) : std::string_view{ head.reason };
        result.bytes = std::format("{} {} {}\r\n", version, status, reason);
        append_outgoing_headers(result, head.headers, head.version, options, bodyless);
        return result;
    }

    /**
     * @brief Encode an HTTP/1.1 chunked body without borrowing any supplied body bytes.
     * @note Empty Encode calls do not terminate the body. A successful Finish is the only terminal
     *       operation; later Encode/Finish calls fail. The caller serializes all access per stream.
     * @internal
     */
    class ChunkEncoder {
    private:
        Headers m_initial_headers; ///< Owned initial fields used to reject connection-specific trailers.
        bool m_finished{}; ///< Whether the terminal chunk has already been emitted.

    public:
        /**
         * @brief Own initial fields for validating any eventual trailers.
         * @param initial_headers Initial fields, including Connection nominations, copied or moved.
         */
        explicit ChunkEncoder(Headers initial_headers = {}) :
            m_initial_headers{ std::move(initial_headers) } {}

        /**
         * @brief Encode one body segment as an owned chunk, or nothing for an empty segment.
         * @param chunk Borrowed binary body bytes, needed only during this call.
         * @return Owned wire bytes, or InvalidRequest after successful Finish.
         */
        [[nodiscard]] auto Encode(std::string_view chunk) -> Result<std::string> {
            if (m_finished) { return std::unexpected{ Error::InvalidRequest }; }
            if (chunk.empty()) { return std::string{}; }
            auto result = std::format("{:x}\r\n", chunk.size());
            result += chunk;
            result += "\r\n";
            return result;
        }

        /**
         * @brief Emit the terminal chunk and validated trailing fields once.
         * @param trailers Borrowed validated fields; handle errors from their mutations before calling.
         * @return Owned terminal bytes, InvalidHeaders for forbidden trailers, or InvalidRequest if
         *         already finished. A rejected Finish leaves the encoder open and emits no bytes.
         */
        [[nodiscard]] auto Finish(Headers const& trailers = {}) -> Result<std::string> {
            if (m_finished) { return std::unexpected{ Error::InvalidRequest }; }
            for (auto const& [name, value] : trailers) {
                if (is_forbidden_trailer(name, m_initial_headers)) {
                    return std::unexpected{ Error::InvalidHeaders };
                }
            }
            std::string result{ "0\r\n" };
            for (auto const& [name, value] : trailers) {
                result += name;
                result += ": ";
                result += value;
                result += "\r\n";
            }
            result += "\r\n";
            m_finished = true;
            return result;
        }
    };

} // namespace mhttp::detail
