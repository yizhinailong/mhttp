/**
 * @file rate_limit.cppm
 * @brief Apply shared per-client fixed-window quotas as route or global Before middleware.
 */
export module mhttp:server.rate_limit;

import std;
import :server.impl.rate_limit;
export import :server.rate_limit_options;
export import :server.request;
export import :server.response;

export namespace mhttp::server {

    /**
     * @brief Share bounded per-ClientIp quotas across all copies registered on routes or global middleware.
     * @note Before, TrackedClients, and capacity changes are thread-safe. Keys use Request::ClientIp,
     *       including the configured trusted-proxy policy; forwarding fields are never parsed here.
     *       A distinct Create call owns independent quotas. Memory remains bounded under address rotation.
     */
    class RateLimit {
    private:
        std::shared_ptr<impl::RateLimitImpl> m_impl; ///< Shared synchronized quotas across aspect copies.
        /**
         * @brief Adopt validated state.
         * @param state Owned quota state.
         */
        explicit RateLimit(std::shared_ptr<impl::RateLimitImpl> state) : m_impl{ std::move(state) } {}

    public:
        /**
         * @brief Create a shared fixed-window limiter without starting threads or timers.
         * @param options Owned quota, duration, capacity, and expiry configuration.
         * @return Middleware or InvalidRequest for invalid bounds.
         */
        [[nodiscard]] static auto Create(RateLimitOptions options = {}) -> Result<RateLimit> {
            auto valid = impl::RateLimitImpl::Validate(options);
            if (!valid) { return std::unexpected{ valid.error() }; }
            return RateLimit{ std::make_shared<impl::RateLimitImpl>(std::move(options)) };
        }
        /**
         * @brief Count this client's request, or short-circuit with 429 and a positive Retry-After hint.
         * @param request Borrowed session request, whose ClientIp was resolved before middleware runs.
         * @param response Borrowed response, changed only for a denied request.
         * @return True to continue, false after setting the denial response.
         * @note Rejected requests refresh LRU order and idle age but never extend the fixed quota window.
         */
        auto Before(Request& request, Response& response) const -> bool {
            if (!m_impl) { static_cast<void>(response.SetError(Status::InternalServerError)); return false; }
            auto retry = m_impl->Check(request.ClientIp());
            if (!retry) { return true; }
            static_cast<void>(response.SetError(Status::TooManyRequests));
            static_cast<void>(response.Headers().Set("Retry-After", std::to_string(retry->count())));
            return false;
        }
        /**
         * @brief Observe current retained clients without triggering expiration.
         * @return Shared bucket count, or zero for a moved-from middleware.
         */
        [[nodiscard]] auto TrackedClients() const -> std::size_t {
            return m_impl ? m_impl->TrackedClients() : 0;
        }
        /**
         * @brief Change shared capacity without resetting quotas of retained clients.
         * @param maximum Positive new hard limit; shrinking evicts least recently accessed clients immediately.
         * @return Success, InvalidRequest for zero, or Canceled for a moved-from middleware.
         */
        [[nodiscard]] auto SetMaxTrackedClients(std::size_t maximum) -> Result<void> {
            return m_impl ? m_impl->SetMaxTrackedClients(maximum) : Result<void>{ std::unexpected{ Error::Canceled } };
        }
    };

} // namespace mhttp::server
