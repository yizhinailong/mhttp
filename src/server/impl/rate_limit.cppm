/**
 * @file rate_limit.cppm
 * @brief Count bounded shared client windows with constant-time lookup and access-ordered idle reclamation.
 */
module mhttp:server.impl.rate_limit;

import std;
import :core.impl.io_error;
import :server.rate_limit_options;

namespace mhttp::impl {

    /**
     * @brief Own synchronized quota buckets whose stable list nodes also own map lookup keys.
     * @note Every operation locks only metadata; no timer, network operation, or worker thread is created.
     */
    class RateLimitImpl {
    private:
        using Clock = std::chrono::steady_clock; ///< Monotonic observation clock.
        /**
         * @brief Own one client identity and its quota through list reordering.
         */
        struct Bucket {
            std::string key; ///< Stable owned identity; map keys borrow this storage.
            Clock::time_point window_start; ///< First accepted request of the current fixed window.
            Clock::time_point last_seen; ///< Most recent request, including rejected requests.
            std::uint32_t used{}; ///< Accepted requests, saturating at the configured quota.
        };
        using Buckets = std::list<Bucket>; ///< Oldest-accessed first; stable nodes avoid copying identities.

        server::RateLimitOptions m_options; ///< Validated quota/capacity configuration.
        Clock::duration m_idle_expiration; ///< Resolved idle age; zero disables idle expiry.
        mutable std::mutex m_mutex; ///< Protects all counters, borrowed map keys, and LRU order.
        Buckets m_lru; ///< Owns keys until their map entries have been removed.
        std::unordered_map<std::string_view, Buckets::iterator> m_buckets; ///< Nonowning stable-node index.

        /**
         * @brief Erase one oldest node without leaving a dangling lookup key.
         * @note Caller holds the mutex and the list is nonempty.
         */
        auto evictOldest() -> void {
            m_buckets.erase(m_lru.front().key);
            m_lru.pop_front();
        }
        /**
         * @brief Round a positive retry hint upward so subsecond waits never advertise zero seconds.
         * @param remaining Time before a quota reset or possible capacity recovery.
         * @return At least one whole second.
         */
        static auto retryAfter(Clock::duration remaining) -> std::chrono::seconds {
            return std::max(std::chrono::seconds{ 1 }, std::chrono::ceil<std::chrono::seconds>(remaining));
        }

    public:
        /**
         * @brief Validate all bounds without allocating a bucket or observing a clock.
         * @param options Borrowed factory configuration.
         * @return InvalidRequest for invalid capacities, durations, or enum values.
         */
        static auto Validate(server::RateLimitOptions const& options) -> Result<void> {
            auto const max = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max() / 2);
            if (options.max_requests == 0 || options.max_tracked_clients == 0 || options.window.count() <= 0
                || options.window > max || (options.idle_expiration && (options.idle_expiration->count() < 0
                    || *options.idle_expiration > max))
                || (options.when_full != server::RateLimitCapacityAction::EvictOldest
                    && options.when_full != server::RateLimitCapacityAction::Reject)) {
                return std::unexpected{ Error::InvalidRequest };
            }
            return {};
        }
        /**
         * @brief Own validated options without retaining caller strings or starting background work.
         * @param options Validated owned configuration.
         */
        explicit RateLimitImpl(server::RateLimitOptions options) :
            m_options{ std::move(options) }, m_idle_expiration{ m_options.idle_expiration.value_or(m_options.window) } {}
        /**
         * @brief Admit and count a client or return a bounded positive retry hint.
         * @param key Borrowed normalized client address; copied only for a newly tracked client.
         * @return No value when accepted, otherwise seconds until quota reset or possible capacity recovery.
         */
        auto Check(std::string_view key) -> std::optional<std::chrono::seconds> {
            std::lock_guard lock{ m_mutex };
            auto const now = Clock::now();
            if (m_idle_expiration > Clock::duration::zero()) {
                while (!m_lru.empty() && now - m_lru.front().last_seen >= m_idle_expiration) { evictOldest(); }
            }
            auto it = m_buckets.find(key);
            if (it == m_buckets.end()) {
                if (m_buckets.size() == m_options.max_tracked_clients) {
                    if (m_options.when_full == server::RateLimitCapacityAction::Reject) {
                        auto const delay = m_idle_expiration > Clock::duration::zero()
                            ? m_idle_expiration - (now - m_lru.front().last_seen) : m_options.window;
                        return retryAfter(delay);
                    }
                    evictOldest();
                }
                m_lru.push_back({ std::string{ key }, now, now });
                try { it = m_buckets.emplace(m_lru.back().key, std::prev(m_lru.end())).first; }
                catch (...) { m_lru.pop_back(); throw; }
            }
            auto& bucket = *it->second;
            bucket.last_seen = now;
            m_lru.splice(m_lru.end(), m_lru, it->second);
            if (now - bucket.window_start >= m_options.window) { bucket.window_start = now; bucket.used = 0; }
            if (bucket.used == m_options.max_requests) { return retryAfter(m_options.window - (now - bucket.window_start)); }
            ++bucket.used;
            return std::nullopt;
        }
        /**
         * @brief Inspect retained bucket count without mutating expiry or quota state.
         * @return Count at this instant, always within the current capacity.
         */
        [[nodiscard]] auto TrackedClients() const -> std::size_t {
            std::lock_guard lock{ m_mutex };
            return m_buckets.size();
        }
        /**
         * @brief Adjust capacity, retaining the newest buckets and their existing quota counts.
         * @param maximum Positive new hard capacity.
         * @return Success or InvalidRequest; invalid input preserves all state.
         */
        auto SetMaxTrackedClients(std::size_t maximum) -> Result<void> {
            if (maximum == 0) { return std::unexpected{ Error::InvalidRequest }; }
            std::lock_guard lock{ m_mutex };
            m_options.max_tracked_clients = maximum;
            while (m_buckets.size() > maximum) { evictOldest(); }
            return {};
        }
    };

} // namespace mhttp::impl
