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
        requires std::same_as<HeadType, RequestHead>
    class MessageParser {
    private:
        /**
         * @brief Track the next expected wire element.
         */
        enum class State {
            StartLine, Headers, FixedBody, ChunkSize, ChunkData, ChunkCr, ChunkLf, Trailers,
            Complete, Failed
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
        std::optional<ProtocolFailure> m_failure; ///< Stable terminal failure until Reset.

    public:
        /**
         * @brief Construct a parser without reading input or allocating body storage.
         * @param limits Bounds applied to each message.
         */
        explicit MessageParser(ParserLimits limits = {}) : m_limits{ limits } {}

        /**
         * @brief Feed input until one observable event or input exhaustion.
         * @param input Borrowed contiguous bytes; may contain any number of successor messages.
         * @return Event and consumed prefix. Call again with the remainder, including an empty span
         *         when the previous Body event delivered the final body bytes.
         */
        [[nodiscard]] auto Feed(std::span<char const> input) -> ParseStep {
            std::size_t consumed{};
            for (;;) {
                if (m_state == State::Failed) {
                    return { ParseEvent::Failed, consumed, {}, m_failure };
                }
                if (m_state == State::Complete) { return { ParseEvent::Complete, consumed }; }
                if (m_state == State::FixedBody || m_state == State::ChunkData) {
                    if (consumed == input.size()) { return { ParseEvent::NeedMore, consumed }; }
                    auto const count = static_cast<std::size_t>(std::min<std::uint64_t>(
                        m_remaining, input.size() - consumed));
                    auto const data = input.subspan(consumed, count);
                    m_remaining -= count;
                    m_body_bytes += count;
                    consumed += count;
                    if (m_remaining == 0) {
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
         * @return Completion if the message already ended, otherwise a stable ConnectionClosed failure.
         */
        [[nodiscard]] auto Finish() -> ParseStep {
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
            m_failure.reset();
        }

        /**
         * @brief Borrow the current message metadata after HeadersComplete.
         * @return Reference valid until Reset or destruction.
         */
        [[nodiscard]] auto Head() const noexcept -> HeadType const& { return m_head; }

        /**
         * @brief Borrow allowed trailers after Complete.
         * @return Reference valid until Reset or destruction.
         */
        [[nodiscard]] auto Trailers() const noexcept -> Headers const& { return m_trailers; }

        /**
         * @brief Inspect validated framing after HeadersComplete.
         * @return Reference valid until Reset or destruction.
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
         * @brief Validate and own a complete, strictly SP-separated request line.
         * @return Success or the fatal request-line error.
         */
        auto parseStartLine() -> ProtocolResult<void> {
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
                if (m_line.empty()) { return std::nullopt; }
                auto const parsed = parseStartLine();
                if (!parsed) { fail(parsed.error()); }
                else { m_state = State::Headers; }
            } else if (m_state == State::Headers && m_line.empty()) {
                auto const framing = analyze_framing(m_head.version, m_head.headers, m_limits);
                if (!framing) { fail(framing.error()); return std::nullopt; }
                if (m_head.absolute_authority) {
                    static_cast<void>(m_head.headers.Set("Host", *m_head.absolute_authority));
                }
                m_framing = *framing;
                m_remaining = m_framing.content_length;
                m_state = m_framing.kind == BodyKind::Chunked ? State::ChunkSize
                    : m_framing.kind == BodyKind::ContentLength ? State::FixedBody : State::Complete;
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

} // namespace mhttp::detail
