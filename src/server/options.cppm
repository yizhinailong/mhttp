/**
 * @file options.cppm
 * @brief Define bounded buffered-server configuration independently of the transport backend.
 */
export module mhttp:server.options;

import std;
export import :core.message;
export import :server.trusted_proxies;

export namespace mhttp::server {

    /**
     * @brief Configure request bounds, operation deadlines, persistence, and buffered response output.
     * @note A server owns a configuration snapshot; changing the original value does not reconfigure
     *       an active server. Timeouts, URI/header bounds, request count, and response bound must be
     *       positive, including max_connections. A zero body bound accepts only empty decoded bodies.
     */
    struct ServerOptions {
        std::chrono::milliseconds read_timeout{ 30'000 }; ///< Deadline for an entire head or body phase.
        std::chrono::milliseconds write_timeout{ 30'000 }; ///< Deadline for an entire response write.
        std::chrono::milliseconds keep_alive_timeout{ 5'000 }; ///< Deadline for a successor request head.
        std::size_t keep_alive_max_requests{ 100 }; ///< Maximum responses per connection, including the current one.
        std::size_t uri_bytes{ 8 * 1024 }; ///< Maximum encoded request-target length.
        std::size_t header_bytes{ 64 * 1024 }; ///< Request fields and trailers together, including delimiters.
        std::size_t header_fields{ 100 }; ///< Request fields and trailers together, including discarded fields.
        std::uint64_t body_bytes{ 8 * 1024 * 1024 }; ///< Maximum decoded buffered request body.
        std::size_t max_response_bytes{ 16 * 1024 * 1024 }; ///< Maximum complete buffered wire response.
        std::size_t max_connections{ 1024 }; ///< Maximum registered sessions, including idle/closing sessions.
        bool reuse_address{ true }; ///< Reuse closed POSIX bindings; Windows always reserves exclusively.
        bool tcp_no_delay{ true }; ///< Disable Nagle on each accepted socket before session startup.
        bool ipv6_only{}; ///< Restrict IPv6 listeners to IPv6; false enables IPv4-mapped dual stack.
        Headers default_headers; ///< Fallback response fields; explicit application fields take precedence.
        TrustedProxies trusted_proxies; ///< Immutable client-IP policy; default ignores forwarded addresses.
    };

} // namespace mhttp::server

export namespace mhttp::detail {

    /**
     * @brief Reject unbounded/immediate configuration before creating server resources.
     * @param options Borrowed configuration snapshot.
     * @return Success, or InvalidRequest for nonpositive limits or unsafe duration conversion.
     * @internal
     */
    [[nodiscard]] auto validate_server_options(
        server::ServerOptions const& options
    ) -> Result<void> {
        auto const max_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::duration::max() / 2);
        auto valid_timeout = [max_timeout](std::chrono::milliseconds value) {
            return value.count() > 0 && value <= max_timeout;
        };
        if (!valid_timeout(options.read_timeout) || !valid_timeout(options.write_timeout)
            || !valid_timeout(options.keep_alive_timeout) || options.keep_alive_max_requests == 0
            || options.uri_bytes == 0 || options.header_bytes == 0 || options.header_fields == 0
            || options.max_response_bytes == 0 || options.max_connections == 0
            || options.body_bytes > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected{ Error::InvalidRequest };
        }
        return {};
    }

    /**
     * @brief Map validated server admission bounds to the shared protocol parser.
     * @param options Validated configuration snapshot.
     * @return Parser limits; individual/start/chunk-line guards retain their protocol defaults.
     * @internal
     */
    [[nodiscard]] auto server_parser_limits(server::ServerOptions const& options) -> ParserLimits {
        return { .uri_bytes = options.uri_bytes, .header_bytes = options.header_bytes,
            .header_fields = options.header_fields, .body_bytes = options.body_bytes };
    }

} // namespace mhttp::detail
