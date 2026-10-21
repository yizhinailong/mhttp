/**
 * @file request_input.cppm
 * @brief Own incremental request input and pipelined bytes independently of a parser's lifetime.
 */
module mhttp:server.impl.request_input;

import std;
import asio;
import :core.impl.connection;
import :core.parser;

namespace mhttp::impl {

    /**
     * @brief Retain one bounded socket buffer shared by sequential request parsers on the connection strand.
     * @note A caller must consume each returned body view before the next ReadEvent. No concurrent reads
     *       are permitted; a request reader must retire before its successor may access this input.
     */
    class RequestInput {
    private:
        std::shared_ptr<Connection> m_connection; ///< Owned socket and serialized executor.
        std::shared_ptr<std::atomic<bool>> m_admission_closed; ///< Optional generation gate for new request heads.
        std::string m_input; ///< Current transport chunk, including any pipelined successor bytes.
        std::size_t m_offset{}; ///< Consumed prefix of the current chunk.
        std::chrono::steady_clock::time_point m_started{}; ///< First nonempty input for this request.

    public:
        /**
         * @brief Retain the connection before publishing input ownership.
         * @param connection Initialized socket; all reads use its strand.
         * @param admission_closed Optional shared gate; existing requests may continue after it closes.
         */
        explicit RequestInput(
            std::shared_ptr<Connection> connection,
            std::shared_ptr<std::atomic<bool>> admission_closed = {}
        ) : m_connection{ std::move(connection) }, m_admission_closed{ std::move(admission_closed) } {}

        /**
         * @brief Begin timing a successor without discarding retained pipelined bytes.
         */
        auto BeginRequest() noexcept -> void { m_started = {}; }

        /**
         * @brief Inspect when this request first supplied input.
         * @return Monotonic timestamp, or the default value before any bytes arrive.
         */
        [[nodiscard]] auto Started() const noexcept -> std::chrono::steady_clock::time_point { return m_started; }

        /**
         * @brief Feed retained bytes before reading, preserving one absolute phase deadline.
         * @param parser Borrowed parser; returned views borrow this input until the next call.
         * @param deadline Absolute deadline for the complete header or body phase.
         * @param max_input Positive bound on each parser feed, optionally limiting decoded body chunks.
         * @return Next observable parser event, or a contextual transport failure.
         */
        auto ReadEvent(
            detail::RequestParser& parser,
            std::chrono::steady_clock::time_point deadline,
            std::size_t max_input = std::numeric_limits<std::size_t>::max()
        ) -> asio::awaitable<Result<detail::ParseStep>> {
            for (;;) {
                auto input = std::span<char const>{ m_input.data(), m_input.size() }.subspan(m_offset);
                input = input.first(std::min(input.size(), max_input));
                if (m_started == std::chrono::steady_clock::time_point{} && !input.empty()) {
                    if (m_admission_closed && m_admission_closed->load()) {
                        co_return std::unexpected{ Error::Canceled };
                    }
                    m_started = std::chrono::steady_clock::now();
                }
                auto step = parser.Feed(input);
                m_offset += step.consumed;
                if (step.event != detail::ParseEvent::NeedMore) { co_return step; }
                if (m_offset < m_input.size()) { continue; }
                auto bytes = co_await m_connection->ReadSome(deadline - std::chrono::steady_clock::now(), false);
                if (!bytes) { co_return std::unexpected{ bytes.error() }; }
                m_input = std::move(*bytes);
                m_offset = 0;
            }
        }
    };

} // namespace mhttp::impl
