/**
 * @file logging.cppm
 * @brief Define owned server logging callbacks using only standard and mhttp types.
 */
export module mhttp:server.logging;

import std;
export import :core.function;
export import :server.request;
export import :server.response;

export namespace mhttp::server {

    /**
     * @brief Identify the operation that produced a diagnostic without relying on message parsing.
     */
    enum class LogEvent {
        BindFailure, ///< Address resolution, listener configuration, or binding failed.
        AcceptFailure, ///< Listener startup or its accept loop failed unexpectedly.
        ConnectionRejected, ///< Capacity, socket initialization, or session creation rejected an accepted socket.
        SessionFailure, ///< Request processing, response preparation, or transport failed.
        AccessLoggerFailure, ///< A completed response's access callback threw; HTTP processing continues.
    };

    /**
     * @brief Own diagnostic metadata that may safely be copied and retained after the callback returns.
     */
    struct LogRecord {
        LogEvent event; ///< Operation context.
        mhttp::Error error; ///< Backend-independent failure category.
        std::error_code native_error; ///< Original platform error where available, otherwise clear.
        std::exception_ptr exception; ///< Contained callback/internal exception, otherwise empty.
        std::optional<Endpoint> remote; ///< Owned peer endpoint if established and available.
        std::optional<Endpoint> local; ///< Owned local endpoint, or requested bind target on BindFailure.
    };

    /**
     * @brief Observe a failure on the bind caller, server strand, or connection strand without throwing outward.
     * @note Record references borrow only this call; copy the owned record to retain it. Callback state
     *       can be shared by concurrent sessions, so synchronize it and avoid blocking I/O execution.
     *       Move-only captures are supported. Logger exceptions are swallowed without recursive logging.
     */
    using Logger = MoveOnlyFunction<void(LogRecord const&)>;

    /**
     * @brief Observe a successfully written final response with the elapsed request duration.
     * @note Request/Response references are borrowed only during this synchronous call. Elapsed time
     *       starts with the first parsed request bytes and ends after the complete response write,
     *       excluding keep-alive idle time and the logger itself. The response is the final application
     *       representation; automatic framing fields are generated separately for the wire.
     *       Calls on different sessions can overlap. Keep callbacks short, synchronize shared state,
     *       and copy any metadata retained after return. Move-only callback captures are supported.
     */
    using AccessLogger = MoveOnlyFunction<
        void(Request const&, Response const&, std::chrono::steady_clock::duration)>;

} // namespace mhttp::server
