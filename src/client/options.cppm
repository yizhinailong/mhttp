/**
 * @file options.cppm
 * @brief Define bounded buffered-client configuration independently of the network backend.
 */
export module mhttp:client.options;

import std;
import :utils.string;
export import :core.message;
export import :core.version;

export namespace mhttp::client {

    /**
     * @brief Choose independent phase budgets, one Send budget, per-operation budgets, or cancellation only.
     */
    enum class TimeoutPolicy {
        Phase, ///< Use connect_timeout, write_timeout, and read_timeout for each complete hop phase.
        Overall, ///< Use timeout once for the entire Send, including connection attempts and redirects.
        Step, ///< Use timeout for connection establishment, each head/body write, and each read operation.
        Never, ///< Disable all automatic client deadlines; explicit/runtime cancellation remains effective.
    };

    /**
     * @brief Configure I/O deadlines, upload negotiation, buffered limits, and fallback fields.
     * @note A client owns a configuration snapshot. All timeouts must be positive. Admission limits
     *       must be positive except body_bytes, where zero accepts only empty responses. Zero
     *       expect_continue_threshold disables automatic negotiation. An empty user_agent disables the
     *       built-in field; explicit request fields override default_headers and the built-in agent.
     */
    struct ClientOptions {
        std::chrono::milliseconds connect_timeout{ 30'000 }; ///< One deadline for DNS and all endpoint attempts.
        std::chrono::milliseconds read_timeout{ 30'000 }; ///< One deadline for the complete response read phase.
        std::chrono::milliseconds write_timeout{ 30'000 }; ///< One deadline for the complete request write phase.
        std::size_t header_bytes{ 64 * 1024 }; ///< Response fields and trailers together, including delimiters.
        std::size_t header_fields{ 100 }; ///< Response fields and trailers together, including discarded fields.
        std::uint64_t body_bytes{ 8 * 1024 * 1024 }; ///< Maximum decoded response body, buffered or streamed.
        std::size_t max_request_bytes{ 16 * 1024 * 1024 }; ///< Maximum complete buffered wire request.
        bool keep_alive{ true }; ///< Permit connection reuse unless either peer requests closure.
        std::string user_agent{ std::string{ "mhttp/" } + std::string{ VERSION } }; ///< Validated optional fallback.
        Headers default_headers; ///< Ordered fallback fields; every explicit request name takes precedence.
        std::size_t max_redirects{}; ///< Maximum followed redirects; zero returns 3xx responses unchanged.
        TimeoutPolicy timeout_policy{ TimeoutPolicy::Phase }; ///< Preserve separately configured phase deadlines by default.
        std::chrono::milliseconds timeout{ 30'000 }; ///< Positive budget used by Overall and Step.
        std::size_t expect_continue_threshold{}; ///< Add Expect for nonempty bodies at least this large; zero disables automatic insertion.
        std::chrono::milliseconds expect_continue_timeout{ 1'000 }; ///< Positive maximum handshake wait before sending the body without 100.
        std::size_t response_chunk_bytes{ 16 * 1024 }; ///< Maximum decoded bytes passed to one response receiver; positive.
        std::size_t upload_chunk_bytes{ 64 * 1024 }; ///< Maximum decoded bytes per upload/progress segment; positive.
    };

} // namespace mhttp::client

export namespace mhttp::detail {

    /**
     * @brief Reject invalid configuration before creating network resources or serializing requests.
     * @param options Borrowed configuration snapshot.
     * @return Success, InvalidRequest for unsafe limits/durations, or InvalidHeaders for user_agent.
     * @internal
     */
    [[nodiscard]] auto validate_client_options(client::ClientOptions const& options) -> Result<void> {
        auto const max_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::duration::max() / 2);
        auto valid_timeout = [max_timeout](std::chrono::milliseconds value) {
            return value.count() > 0 && value <= max_timeout;
        };
        if (!valid_timeout(options.connect_timeout) || !valid_timeout(options.read_timeout)
            || !valid_timeout(options.write_timeout) || !valid_timeout(options.timeout)
            || !valid_timeout(options.expect_continue_timeout) || options.header_bytes == 0
            || options.header_fields == 0 || options.max_request_bytes == 0
            || options.response_chunk_bytes == 0 || options.upload_chunk_bytes == 0
            || options.body_bytes > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{ Error::InvalidRequest };
        }
        switch (options.timeout_policy) {
        case client::TimeoutPolicy::Phase: case client::TimeoutPolicy::Overall:
        case client::TimeoutPolicy::Step: case client::TimeoutPolicy::Never: break;
        default: return std::unexpected{ Error::InvalidRequest };
        }
        if (!utils::is_field_value(options.user_agent)) { return std::unexpected{ Error::InvalidHeaders }; }
        return {};
    }

    /**
     * @brief Map buffered response admission bounds onto the shared incremental parser.
     * @param options Validated client configuration.
     * @return Parser bounds; individual/start/chunk-line guards keep their protocol defaults.
     * @internal
     */
    [[nodiscard]] auto client_parser_limits(client::ClientOptions const& options) -> ParserLimits {
        return { .header_bytes = options.header_bytes, .header_fields = options.header_fields,
            .body_bytes = options.body_bytes };
    }

} // namespace mhttp::detail
