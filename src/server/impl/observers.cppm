/**
 * @file observers.cppm
 * @brief Retain immutable observer identities and isolate synchronous callback failures from HTTP flow.
 */
module mhttp:server.impl.observers;

import std;
import :server.logging;

namespace mhttp::impl {

    /**
     * @brief Snapshot owned callback identities before starting a listener or session.
     * @note Configuration never changes while a generation runs. User callback state may be mutable
     *       and shared by multiple sessions; applications must provide their own synchronization.
     */
    struct ServerObservers {
        std::shared_ptr<server::Logger> logger; ///< Optional owned diagnostic observer.
        std::shared_ptr<server::AccessLogger> access_logger; ///< Optional owned access observer.

        /**
         * @brief Construct owned diagnostic metadata and contain every observer/allocation failure.
         * @param event Operation context.
         * @param error Backend-independent reason.
         * @param native_error Optional native platform failure.
         * @param exception Optional contained callback failure.
         * @param remote Borrowed endpoint copied into the record if present.
         * @param local Borrowed endpoint copied into the record if present.
         */
        auto Log(
            server::LogEvent event,
            Error error,
            std::error_code native_error = {},
            std::exception_ptr exception = {},
            Endpoint const* remote = nullptr,
            Endpoint const* local = nullptr
        ) const noexcept -> void {
            if (!logger) { return; }
            try {
                server::LogRecord record{ event, error, native_error, std::move(exception) };
                if (remote) { record.remote = *remote; }
                if (local) { record.local = *local; }
                (*logger)(record);
            } catch (...) {} // Never recursively log a logger failure.
        }

        /**
         * @brief Observe one complete final write without changing response or connection policy on failure.
         * @param request Request borrowed until callback return.
         * @param response Final application response borrowed until callback return.
         * @param elapsed Time since the first request bytes, excluding this callback.
         */
        auto Access(
            server::Request const& request,
            server::Response const& response,
            std::chrono::steady_clock::duration elapsed
        ) const noexcept -> void {
            if (!access_logger) { return; }
            try { (*access_logger)(request, response, elapsed); }
            catch (...) {
                Log(server::LogEvent::AccessLoggerFailure, Error::Unknown, {}, std::current_exception(),
                    &request.RemoteEndpoint(), &request.LocalEndpoint());
            }
        }
    };

} // namespace mhttp::impl
