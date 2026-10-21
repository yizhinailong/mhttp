/**
 * @file rate_limit_options.cppm
 * @brief Configure fixed-window per-client quotas and bounded client tracking.
 */
export module mhttp:server.rate_limit_options;

import std;

export namespace mhttp::server {

    /**
     * @brief Select admission for unseen clients when the tracking table reaches capacity.
     */
    enum class RateLimitCapacityAction {
        EvictOldest, ///< Evict the least recently accessed client and count the new client's request.
        Reject, ///< Return 429 for unseen clients while every retained bucket remains occupied.
    };

    /**
     * @brief Configure a shared per-ClientIp fixed-window limiter with bounded memory.
     * @note A window starts with that client's first request. Idle expiration may reset its quota sooner
     *       if configured shorter than the window. Rejected requests also refresh the client's last access.
     */
    struct RateLimitOptions {
        std::uint32_t max_requests{ 100 }; ///< Positive accepted requests per client window.
        std::chrono::milliseconds window{ 1'000 }; ///< Positive monotonic window duration.
        std::size_t max_tracked_clients{ 8192 }; ///< Positive hard limit on retained client buckets.
        std::optional<std::chrono::milliseconds> idle_expiration; ///< Absent uses window; zero disables idle expiry.
        RateLimitCapacityAction when_full{ RateLimitCapacityAction::EvictOldest }; ///< Unseen-client capacity policy.
    };

} // namespace mhttp::server
