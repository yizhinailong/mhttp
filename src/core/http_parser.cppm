/**
 * @file http_parser.cppm
 * @brief Incrementally parse bounded HTTP messages without copying body or pipelined bytes.
 */
export module mhttp:core.parser;

import std;
import :utils.string;
import :core.url;
export import :core.message;

export namespace mhttp::detail {

    /**
     * @brief Parse one HTTP message through explicit header, body, and completion events.
     * @tparam HeadType Owned start-line and header metadata.
     * @note The caller retains all input, advances by ParseStep::consumed, and keeps body views alive
     *       until their consumers finish. Complete consumes no successor bytes. Reset is only for a
     *       successfully completed message; every Failed event requires closing the connection.
     * @internal
     */
    template <typename HeadType>
        requires (std::same_as<HeadType, RequestHead> || std::same_as<HeadType, ResponseHead>)
    class MessageParser {
    private:
        /**
         * @brief Track the next expected wire element.
         */
        enum class State {
            StartLine, Headers, FixedBody, UntilEof, ChunkSize, ChunkData, ChunkCr, ChunkLf, Trailers,
            Informational, Complete, Failed
        };

        ParserLimits m_limits; ///< Immutable limits for this parser's messages.
        HeadType m_head; ///< Owned metadata for the current message.
        Headers m_trailers; ///< Allowed trailers, separate from initial headers.
        MessageFraming m_framing; ///< Validated body and persistence semantics.
        State m_state{ State::StartLine }; ///< Current decoding state.
        std::string m_line; ///< Only the incomplete current line is buffered.
        bool m_saw_cr{}; ///< A CR must be followed by LF, even across Feed calls.
        std::size_t m_header_bytes{}; ///< Initial fields and trailers, including line terminators.
        std::size_t m_field_count{}; ///< Initial and trailer field count, including discarded trailers.
        std::size_t m_target_bytes{}; ///< Incremental target byte count for early URI rejection.
        unsigned m_start_part{}; ///< Method, target, or version part while reading the start line.
        std::uint64_t m_remaining{}; ///< Remaining fixed-body or current chunk bytes.
        std::uint64_t m_body_bytes{}; ///< Decoded body bytes delivered to the caller.
        Method m_response_to{ Method::Get }; ///< Request method determining response body semantics.
        std::size_t m_interim_responses{}; ///< Informational responses received before the final response.
        bool m_emit_informational{}; ///< Expose each complete informational head instead of silently skipping it.
        std::optional<ProtocolFailure> m_failure; ///< Stable terminal failure until Reset.

    public:
        /**
         * @brief Construct a parser without reading input or allocating body storage.
         * @param limits Bounds applied to each message.
         */
        explicit MessageParser(ParserLimits limits = {}) : m_limits{ limits } {}

        /**
         * @brief Construct a response parser with the originating request's method.
         * @param response_to Method that caused the response; HEAD never carries a wire body.
         * @param limits Bounds applied to each message, with a separate informational-response limit.
         * @param emit_informational Emit Informational before resetting each 1xx head on the next Feed call.
         */
        explicit MessageParser(
            Method response_to,
            ParserLimits limits = {},
            bool emit_informational = false
        ) requires std::same_as<HeadType, ResponseHead> :
            m_limits{ limits }, m_response_to{ response_to }, m_emit_informational{ emit_informational } {}

        /**
         * @brief Feed input until one observable event or input exhaustion.
         * @param input Borrowed contiguous bytes; may contain any number of successor messages.
         * @return Event and consumed prefix. Call again with the remainder, including an empty span
         *         when the previous Body event delivered the final body bytes.
         */
        [[nodiscard]] auto Feed(std::span<char const> input) -> ParseStep {
            std::size_t consumed{};
            for (;;) {
                if (m_state == State::Informational) {
                    m_head = HeadType{};
                    m_framing = {};
                    m_state = State::StartLine;
                    m_header_bytes = 0;
                    m_field_count = 0;
                }
                if (m_state == State::Failed) {
                    return { ParseEvent::Failed, consumed, {}, m_failure };
                }
                if (m_state == State::Complete) { return { ParseEvent::Complete, consumed }; }
                if (m_state == State::FixedBody || m_state == State::ChunkData || m_state == State::UntilEof) {
                    if (consumed == input.size()) { return { ParseEvent::NeedMore, consumed }; }
                    bool const until_eof = m_state == State::UntilEof;
                    auto const available = until_eof ? m_limits.body_bytes - m_body_bytes : m_remaining;
                    if (until_eof && available == 0) {
                        fail({ Error::BodyTooLarge, Status::ContentTooLarge });
                        continue;
                    }
                    auto const count = static_cast<std::size_t>(std::min<std::uint64_t>(
                        available, input.size() - consumed));
                    auto const data = input.subspan(consumed, count);
                    if (!until_eof) { m_remaining -= count; }
                    m_body_bytes += count;
                    consumed += count;
                    if (!until_eof && m_remaining == 0) {
                        m_state = m_state == State::FixedBody ? State::Complete : State::ChunkCr;
                    }
                    return { ParseEvent::Body, consumed, data };
                }
                if (consumed == input.size()) { return { ParseEvent::NeedMore, consumed }; }
                if (m_state == State::ChunkCr || m_state == State::ChunkLf) {
                    char const expected = m_state == State::ChunkCr ? '\r' : '\n';
                    if (input[consumed++] != expected) { fail({}); continue; }
                    m_state = m_state == State::ChunkCr ? State::ChunkLf : State::ChunkSize;
                    continue;
                }
                bool const field_line = m_state == State::Headers || m_state == State::Trailers;
                if (field_line) {
                    if (m_header_bytes == m_limits.header_bytes) {
                        fail({ Error::HeaderTooLarge, Status::RequestHeaderFieldsTooLarge });
                        continue;
                    }
                    ++m_header_bytes;
                }
                char const byte = input[consumed++];
                if (m_saw_cr) {
                    m_saw_cr = false;
                    if (byte != '\n') { fail({}); continue; }
                    auto const event = handleLine();
                    m_line.clear();
                    if (event) { return { *event, consumed }; }
                    continue;
                }
                if (byte == '\n') { fail({}); continue; }
                if (byte == '\r') { m_saw_cr = true; continue; }
                if constexpr (std::same_as<HeadType, RequestHead>) {
                    if (m_state == State::StartLine) {
                        if (byte == ' ') {
                            if (m_start_part < 2) { ++m_start_part; }
                        } else if (m_start_part == 1) {
                            if (m_target_bytes == m_limits.uri_bytes) {
                                fail({ Error::InvalidRequest, Status::UriTooLong });
                                continue;
                            }
                            ++m_target_bytes;
                        }
                    }
                }
                auto const line_limit = field_line ? m_limits.header_line_bytes
                    : m_state == State::ChunkSize ? m_limits.chunk_line_bytes : m_limits.start_line_bytes;
                if (m_line.size() == line_limit) {
                    fail(field_line ? ProtocolFailure{ Error::HeaderTooLarge, Status::RequestHeaderFieldsTooLarge }
                                    : ProtocolFailure{});
                    continue;
                }
                m_line.push_back(byte);
            }
        }

        /**
         * @brief Signal that the peer closed the input stream.
         * @return Completion for an ended or EOF-delimited message, otherwise ConnectionClosed failure.
         */
        [[nodiscard]] auto Finish() -> ParseStep {
            if (m_state == State::UntilEof) { m_state = State::Complete; }
            if (m_state != State::Complete && m_state != State::Failed) {
                fail({ Error::ConnectionClosed, Status::BadRequest });
            }
            return Feed({});
        }

        /**
         * @brief Prepare to parse a successor after successfully completing a message.
         * @note Preserve the caller's unconsumed input and feed it after resetting. Never use Reset
         *       to recover a failed connection: its framing can no longer be trusted.
         */
        auto Reset() -> void {
            m_head = HeadType{};
            m_trailers.Clear();
            m_framing = {};
            m_state = State::StartLine;
            m_line.clear();
            m_saw_cr = false;
            m_header_bytes = 0;
            m_field_count = 0;
            m_target_bytes = 0;
            m_start_part = 0;
            m_remaining = 0;
            m_body_bytes = 0;
            m_interim_responses = 0;
            m_failure.reset();
        }

        /**
         * @brief Borrow final metadata after HeadersComplete, or one 1xx head after Informational.
         * @return Final metadata remains valid until Reset/destruction; an informational head only until the next Feed.
         */
        [[nodiscard]] auto Head() const noexcept -> HeadType const& { return m_head; }

        /**
         * @brief Borrow allowed trailers after Complete.
         * @return Reference valid until Reset or destruction.
         */
        [[nodiscard]] auto Trailers() const noexcept -> Headers const& { return m_trailers; }

        /**
         * @brief Inspect validated framing after HeadersComplete or Informational.
         * @return Final framing remains valid until Reset/destruction; informational framing only until the next Feed.
         */
        [[nodiscard]] auto Framing() const noexcept -> MessageFraming const& { return m_framing; }

        /**
         * @brief Count decoded bytes already returned as Body events.
         * @return Delivered body size without counting framing bytes.
         */
        [[nodiscard]] auto BodyBytes() const noexcept -> std::uint64_t { return m_body_bytes; }

    private:
        /**
         * @brief Enter a terminal failure state without consuming any successor bytes.
         * @param failure Fatal error and suggested server response.
         */
        auto fail(ProtocolFailure failure) -> void {
            m_failure = failure;
            m_state = State::Failed;
        }

        /**
         * @brief Dispatch start-line validation according to the message metadata type.
         * @return Success or a fatal start-line error.
         */
        auto parseStartLine() -> ProtocolResult<void> {
            if constexpr (std::same_as<HeadType, ResponseHead>) {
                return parseResponseStartLine();
            } else {
                return parseRequestStartLine();
            }
        }

        /**
         * @brief Validate and own a complete, strictly SP-separated request line.
         * @return Success or the fatal request-line error.
         */
        auto parseRequestStartLine() -> ProtocolResult<void> {
            auto const bad = std::unexpected{ ProtocolFailure{} };
            std::string_view const line{ m_line };
            if (line == "PRI * HTTP/2.0") { return bad; }
            auto const first = line.find(' ');
            if (first == std::string_view::npos) { return bad; }
            auto const second = line.find(' ', first + 1);
            if (second == std::string_view::npos || line.find(' ', second + 1) != std::string_view::npos) {
                return bad;
            }
            auto const method = line.substr(0, first);
            auto const target = line.substr(first + 1, second - first - 1);
            auto const version = line.substr(second + 1);
            if (!utils::is_field_name(method) || target.empty()
                || !std::ranges::all_of(target, [](unsigned char byte) {
                    return byte > 0x20 && byte != 0x7f && byte != '#';
                })) { return bad; }
            if (version == "HTTP/1.1") { m_head.version = Version::Http11; }
            else if (version == "HTTP/1.0") { m_head.version = Version::Http10; }
            else { return std::unexpected{ ProtocolFailure{ Error::ProtocolError, Status::HttpVersionNotSupported } }; }
            if (!ParseMethod(method)) {
                return std::unexpected{ ProtocolFailure{ Error::InvalidRequest, Status::NotImplemented } };
            }
            m_head.method = method;
            if (target == "*") {
                if (method != "OPTIONS") { return bad; }
                m_head.target = target;
            } else if (target.starts_with('/')) {
                m_head.target = target;
            } else {
                auto const url = Url::Parse(target);
                if (!url) { return bad; }
                m_head.target = url->Target();
                m_head.absolute_authority = MakeHostValue(url->Host(), url->Port(), url->Scheme());
            }
            return {};
        }

        /**
         * @brief Validate an HTTP status line with a three-digit code and optional reason.
         * @return Success or a fatal status-line error.
         */
        auto parseResponseStartLine() -> ProtocolResult<void> {
            auto const bad = std::unexpected{ ProtocolFailure{} };
            std::string_view const line{ m_line };
            auto const space = line.find(' ');
            if (space == std::string_view::npos) { return bad; }
            auto const version = line.substr(0, space);
            if (version == "HTTP/1.1") { m_head.version = Version::Http11; }
            else if (version == "HTTP/1.0") { m_head.version = Version::Http10; }
            else { return bad; }
            auto const status = line.substr(space + 1);
            if (status.size() < 3 || status[0] < '1' || status[0] > '9'
                || status[1] < '0' || status[1] > '9' || status[2] < '0' || status[2] > '9'
                || (status.size() > 3 && status[3] != ' ')) { return bad; }
            auto const reason = status.size() > 3 ? status.substr(4) : std::string_view{};
            if (!std::ranges::all_of(reason, [](unsigned char byte) {
                return byte == '\t' || (byte >= 0x20 && byte != 0x7f);
            })) { return bad; }
            m_head.status = static_cast<Status>((status[0] - '0') * 100 + (status[1] - '0') * 10 + status[2] - '0');
            m_head.reason = reason;
            return {};
        }

        /**
         * @brief Parse one field while counting even forbidden trailer fields toward limits.
         * @param trailer Whether this is a trailing field.
         * @return Success or a lexical/count failure.
         */
        auto parseField(bool trailer) -> ProtocolResult<void> {
            if (m_field_count == m_limits.header_fields) {
                return std::unexpected{ ProtocolFailure{ Error::HeaderTooLarge, Status::RequestHeaderFieldsTooLarge } };
            }
            ++m_field_count;
            std::string_view const line{ m_line };
            auto const colon = line.find(':');
            if (colon == std::string_view::npos) { return std::unexpected{ ProtocolFailure{} }; }
            auto const name = line.substr(0, colon);
            auto const value = utils::trim_ows(line.substr(colon + 1));
            if (trailer && is_forbidden_trailer(name, m_head.headers)) {
                if (!utils::is_field_name(name) || !utils::is_field_value(value)) {
                    return std::unexpected{ ProtocolFailure{} };
                }
            } else {
                auto const added = (trailer ? m_trailers : m_head.headers).Add(name, value);
                if (!added) { return std::unexpected{ ProtocolFailure{} }; }
            }
            return {};
        }

        /**
         * @brief Validate one chunk length and its bounded, ignored extension text.
         * @return Success or a malformed/oversized-body failure.
         */
        auto parseChunkSize() -> ProtocolResult<void> {
            std::string_view const line{ m_line };
            auto const semicolon = line.find(';');
            if (semicolon != std::string_view::npos
                && !std::ranges::all_of(line.substr(semicolon + 1), [](unsigned char byte) {
                    return byte >= 0x20 && byte != 0x7f;
                })) { return std::unexpected{ ProtocolFailure{} }; }
            auto const length = utils::parse_integer<std::uint64_t>(line.substr(0, semicolon), 16);
            if (!length) { return std::unexpected{ ProtocolFailure{} }; }
            if (*length > m_limits.body_bytes - m_body_bytes) {
                return std::unexpected{ ProtocolFailure{ Error::BodyTooLarge, Status::ContentTooLarge } };
            }
            m_remaining = *length;
            m_state = *length == 0 ? State::Trailers : State::ChunkData;
            return {};
        }

        /**
         * @brief Transition on a complete CRLF-terminated line without retaining a borrowed view.
         * @return A header/completion event, or no event when parsing should continue.
         */
        auto handleLine() -> std::optional<ParseEvent> {
            if (m_state == State::StartLine) {
                if constexpr (std::same_as<HeadType, RequestHead>) {
                    if (m_line.empty()) { return std::nullopt; }
                }
                auto const parsed = parseStartLine();
                if (!parsed) { fail(parsed.error()); }
                else { m_state = State::Headers; }
            } else if (m_state == State::Headers && m_line.empty()) {
                constexpr bool RESPONSE = std::same_as<HeadType, ResponseHead>;
                bool bodyless{};
                if constexpr (RESPONSE) {
                    bodyless = m_response_to == Method::Head || IsInformational(m_head.status)
                        || m_head.status == Status::NoContent || m_head.status == Status::NotModified;
                }
                auto const framing = analyze_framing(m_head.version, m_head.headers, m_limits, RESPONSE, bodyless);
                if (!framing) { fail(framing.error()); return std::nullopt; }
                if constexpr (RESPONSE) {
                    if (IsInformational(m_head.status)) {
                        if (m_interim_responses == m_limits.interim_responses) { fail({}); return std::nullopt; }
                        ++m_interim_responses;
                        if (m_emit_informational) {
                            m_framing = *framing;
                            m_state = State::Informational;
                            return ParseEvent::Informational;
                        }
                        m_head = HeadType{};
                        m_state = State::StartLine;
                        m_header_bytes = 0;
                        m_field_count = 0;
                        return std::nullopt;
                    }
                } else {
                    if (m_head.absolute_authority) {
                        static_cast<void>(m_head.headers.Set("Host", *m_head.absolute_authority));
                    }
                }
                m_framing = *framing;
                m_remaining = m_framing.content_length;
                m_state = m_framing.kind == BodyKind::Chunked ? State::ChunkSize
                    : m_framing.kind == BodyKind::ContentLength ? State::FixedBody
                    : m_framing.kind == BodyKind::UntilEof ? State::UntilEof : State::Complete;
                return ParseEvent::HeadersComplete;
            } else if (m_state == State::Trailers && m_line.empty()) {
                m_state = State::Complete;
                return ParseEvent::Complete;
            } else {
                auto const parsed = m_state == State::ChunkSize ? parseChunkSize()
                    : parseField(m_state == State::Trailers);
                if (!parsed) { fail(parsed.error()); }
            }
            return std::nullopt;
        }
    };

    /**
     * @brief Parse one HTTP request with event-based, zero-copy body delivery.
     * @internal
     */
    using RequestParser = MessageParser<RequestHead>;

    /**
     * @brief Parse a final response, optionally exposing its bounded preceding informational responses.
     * @internal
     */
    using ResponseParser = MessageParser<ResponseHead>;

} // namespace mhttp::detail
