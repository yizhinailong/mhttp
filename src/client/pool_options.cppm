/**
 * @file pool_options.cppm
 * @brief Define bounded client pool capacity, acquisition, and idle maintenance configuration.
 */
export module mhttp:client.pool_options;

import std;
export import :client.options;

export namespace mhttp::client {

    /**
     * @brief Configure independently borrowed single-flight clients grouped by normalized HTTP origin.
     * @note All capacities and check intervals are positive. Zero idle_timeout disables timed eviction.
     *       A slot represents a lazy client object and need not contain an established TCP connection.
     */
    struct ClientPoolOptions {
        std::size_t max_per_origin{ 16 }; ///< Maximum active and idle slots for one scheme/host/port.
        std::size_t max_total{ 100 }; ///< Maximum slots across all origins.
        std::size_t max_waiters{ 1024 }; ///< Maximum queued acquisitions; excess requests return Busy.
        std::chrono::milliseconds idle_timeout{ 60'000 }; ///< Idle age eligible for periodic eviction; zero disables it.
        std::chrono::milliseconds idle_check_interval{ 60'000 }; ///< Positive independent eviction tick interval.
        bool validate_on_borrow{}; ///< Probe reused idle connections before lending them.
        ClientOptions client_options; ///< Configuration snapshot used when creating a new lazy client.
    };

    /**
     * @brief Inspect a consistent pool or origin snapshot from any thread.
     */
    struct ClientPoolStats {
        std::size_t active{}; ///< Outstanding loans, including those retained by issued request Tasks.
        std::size_t idle{}; ///< Returned slots available for reuse or later eviction.
        std::size_t waiting{}; ///< Acquisitions queued for capacity.
        /**
         * @brief Count allocated client slots independently of queued demand.
         * @return Active plus idle slots.
         */
        [[nodiscard]] auto Total() const noexcept -> std::size_t { return active + idle; }
    };

} // namespace mhttp::client
