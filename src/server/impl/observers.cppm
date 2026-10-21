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
        std::shared_ptr<server::AccessLogger> access_logger; ///< Optional owned access observer.

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
            catch (...) {} // Observers cannot alter a completed HTTP response or escape an I/O callback.
        }
    };

} // namespace mhttp::impl
