/**
 * @file logging.cppm
 * @brief Define owned server logging callbacks using only standard and mhttp types.
 */
export module mhttp:server.logging;

import std;
export import :server.request;
export import :server.response;

export namespace mhttp::server {

    /**
     * @brief Observe a successfully written final response with the elapsed request duration.
     * @note Request/Response references are borrowed only during this synchronous call. Elapsed time
     *       starts with the first parsed request bytes and ends after the complete response write,
     *       excluding keep-alive idle time and the logger itself. The response is the final application
     *       representation; automatic framing fields are generated separately for the wire.
     *       Calls on different sessions can overlap. Keep callbacks short, synchronize shared state,
     *       and copy any metadata retained after return. Move-only callback captures are supported.
     */
    using AccessLogger = std::move_only_function<
        void(Request const&, Response const&, std::chrono::steady_clock::duration)>;

} // namespace mhttp::server
