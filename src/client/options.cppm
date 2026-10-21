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
     * @brief Configure connection/read/write deadlines, buffered input/output limits, and fallback fields.
     * @note A client owns a configuration snapshot. All timeouts and limits must be positive except
     *       body_bytes, where zero accepts only empty responses. An empty user_agent disables the
     *       built-in field; explicit request fields override default_headers and the built-in agent.
     */
    struct ClientOptions {
        std::chrono::milliseconds connect_timeout{ 30'000 }; ///< One deadline for DNS and all endpoint attempts.
        std::chrono::milliseconds read_timeout{ 30'000 }; ///< One deadline for the complete response read phase.
        std::chrono::milliseconds write_timeout{ 30'000 }; ///< One deadline for the complete request write phase.
        std::size_t header_bytes{ 64 * 1024 }; ///< Response fields and trailers together, including delimiters.
        std::size_t header_fields{ 100 }; ///< Response fields and trailers together, including discarded fields.
        std::uint64_t body_bytes{ 8 * 1024 * 1024 }; ///< Maximum decoded buffered response body.
        std::size_t max_request_bytes{ 16 * 1024 * 1024 }; ///< Maximum complete buffered wire request.
        bool keep_alive{ true }; ///< Permit connection reuse unless either peer requests closure.
        std::string user_agent{ std::string{ "mhttp/" } + std::string{ VERSION } }; ///< Validated optional fallback.
        Headers default_headers; ///< Ordered fallback fields; every explicit request name takes precedence.
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
            || !valid_timeout(options.write_timeout) || options.header_bytes == 0
            || options.header_fields == 0 || options.max_request_bytes == 0
            || options.body_bytes > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{ Error::InvalidRequest };
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
