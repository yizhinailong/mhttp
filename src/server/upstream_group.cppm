/**
 * @file upstream_group.cppm
 * @brief Select and account for proxy backends with passive failure recovery.
 */
export module mhttp:upstream_group;

import std;
import :strings;
import :url;

export namespace mhttp {

    /**
     * @brief One backend in a proxy route's immutable upstream list.
     */
    struct UpstreamBackend {
        std::string url;           ///< HTTP or WebSocket URL, including an optional base path.
        std::uint32_t weight{ 1 }; ///< Positive weight used by weighted round robin.
    };

    /**
     * @brief Selection policy shared by HTTP and WebSocket forwarding.
     */
    enum class UpstreamStrategy {
        RoundRobin,         ///< Rotate evenly through healthy backends.
        WeightedRoundRobin, ///< Smooth weighted rotation through healthy backends.
        LeastConnections,   ///< Fewest active exchanges, including connect and capacity waits.
    };

    /**
     * @brief Selection and passive recovery settings for a backend-list route.
     */
    struct UpstreamGroupConfig {
        UpstreamStrategy strategy{ UpstreamStrategy::RoundRobin };                         ///< Selection policy.
        std::chrono::steady_clock::duration failure_cooldown{ std::chrono::seconds{ 5 } }; ///< Retry a failed backend after this delay; zero disables cooling.
    };

} // namespace mhttp

namespace mhttp::detail {

    class UpstreamGroup;

    /**
     * @brief Move-only reservation that releases a selected backend exactly once.
     */
    class UpstreamLease {
    private:
        std::shared_ptr<UpstreamGroup> m_group; ///< Keeps immutable URLs and runtime counters alive.
        std::size_t m_index{ 0 };               ///< Selected backend identity, independent of URL spelling.
        std::uint64_t m_generation{ 0 };        ///< Ignore outcomes from before a newer failure.
        bool m_probe{ false };                  ///< Reserve the only half-open probe for this backend.

        friend class UpstreamGroup;

        /**
         * @brief Take ownership of a reservation already counted by the group.
         */
        UpstreamLease(
            std::shared_ptr<UpstreamGroup> group,
            std::size_t index,
            std::uint64_t generation,
            bool probe
        ) : m_group{ std::move(group) }, m_index{ index }, m_generation{ generation }, m_probe{ probe } {}

    public:
        UpstreamLease(UpstreamLease const&) = delete;
        auto operator=(UpstreamLease const&) -> UpstreamLease& = delete;

        /**
         * @brief Transfer a reservation without changing its active count.
         */
        UpstreamLease(
            UpstreamLease&& other
        ) noexcept : m_group{ std::move(other.m_group) }, m_index{ other.m_index }, m_generation{ other.m_generation }, m_probe{ other.m_probe } {}

        /**
         * @brief Release any previous reservation, then transfer the replacement.
         */
        auto operator=(UpstreamLease&& other) noexcept -> UpstreamLease& {
            if (this != &other) {
                Release();
                m_group = std::move(other.m_group);
                m_index = other.m_index;
                m_generation = other.m_generation;
                m_probe = other.m_probe;
            }
            return *this;
        }

        /**
         * @brief Release even on preparation failure or exception unwinding.
         */
        ~UpstreamLease() { Release(); }

        /**
         * @brief Borrow the validated URL while this reservation is held.
         */
        [[nodiscard]] auto Url() const -> url::UrlInfo const&;

        /**
         * @brief Restore a probing backend after response headers or a valid WS handshake.
         */
        auto Success() -> void;

        /**
         * @brief Cool a backend after an attributable upstream I/O or protocol failure.
         */
        auto Failure() -> void;

        /**
         * @brief Release the active count and make an abandoned probe eligible again.
         */
        auto Release() noexcept -> void;
    };

    /**
     * @brief Thread-safe selector shared by all session strands of one proxy route.
     */
    class UpstreamGroup : public std::enable_shared_from_this<UpstreamGroup> {
    private:
        /**
         * @brief Availability of a configured backend.
         */
        enum class Health {
            Healthy, ///< Eligible for ordinary traffic.
            Cooling, ///< Wait until the retry deadline.
            Probing, ///< One request owns the recovery probe.
        };

        /**
         * @brief Immutable endpoint and mutex-protected balancing state.
         */
        struct Backend {
            url::UrlInfo url;                                 ///< Validated origin and normalized base path.
            std::uint32_t weight;                             ///< Positive configured weight.
            std::uint64_t active{ 0 };                        ///< Includes connection and handshake attempts.
            std::int64_t current{ 0 };                        ///< Smooth weighted-round-robin accumulator.
            Health health{ Health::Healthy };                 ///< Current availability.
            std::chrono::steady_clock::time_point retry_at{}; ///< Next passive probe time.
            std::uint64_t generation{ 0 };                    ///< Discards outcomes from stale reservations.
        };

        std::vector<Backend> m_backends; ///< Stable backend identities and runtime counters.
        UpstreamGroupConfig m_config;    ///< Immutable selection and recovery settings.
        std::mutex m_mutex;              ///< Protects selection, accounting, and health as one transaction.
        std::size_t m_cursor{ 0 };       ///< Rotation and least-load tie breaker.
        std::size_t m_probe_cursor{ 0 }; ///< Fair rotation among due recovery probes.

        friend class UpstreamLease;

    public:
        /**
         * @brief Validate every backend before route registration can mutate the router.
         * @throws std::invalid_argument For invalid URLs, weights, policy, or cooldown.
         */
        UpstreamGroup(
            std::vector<UpstreamBackend> backends,
            std::string_view scheme,
            UpstreamGroupConfig config
        ) : m_config{ config } {
            if (backends.empty() || config.failure_cooldown < std::chrono::steady_clock::duration::zero() || (config.strategy != UpstreamStrategy::RoundRobin && config.strategy != UpstreamStrategy::WeightedRoundRobin && config.strategy != UpstreamStrategy::LeastConnections)) {
                throw std::invalid_argument{ "mhttp: upstream group requires backends, a valid strategy, and a nonnegative cooldown" };
            }
            std::int64_t total_weight{ 0 };
            for (auto const& backend : backends) {
                auto parsed{ url::parse_url(backend.url) };
                if (!parsed || !iequals(parsed->scheme, scheme) || parsed->has_query || backend.url.find('#') != std::string::npos || backend.weight == 0 || backend.weight > std::numeric_limits<std::int64_t>::max() / 2 - total_weight) {
                    throw std::invalid_argument{ "mhttp: upstream group requires matching plain URLs and positive bounded weights" };
                }
                while (!parsed->path.empty() && parsed->path.back() == '/') {
                    parsed->path.pop_back();
                }
                for (auto const& previous : m_backends) {
                    if (iequals(previous.url.host, parsed->host) && previous.url.EffectivePort() == parsed->EffectivePort() && previous.url.path == parsed->path) {
                        throw std::invalid_argument{ "mhttp: duplicate upstream backend" };
                    }
                }
                total_weight += backend.weight;
                m_backends.push_back(Backend{ std::move(*parsed), backend.weight });
            }
        }

        /**
         * @brief Atomically select and count one backend; no value means all are unavailable.
         * @note The mutex is never held across I/O or a coroutine suspension.
         */
        [[nodiscard]] auto Acquire() -> std::optional<UpstreamLease> {
            std::lock_guard const lock{ m_mutex };
            auto const now{ std::chrono::steady_clock::now() };
            for (std::size_t offset{ 0 }; offset < m_backends.size(); ++offset) {
                auto const index{ (m_probe_cursor + offset) % m_backends.size() };
                auto& backend{ m_backends[index] };
                if (backend.health == Health::Cooling && backend.retry_at <= now) {
                    backend.health = Health::Probing;
                    ++backend.active;
                    m_probe_cursor = (index + 1) % m_backends.size();
                    return UpstreamLease{ shared_from_this(), index, backend.generation, true };
                }
            }
            auto const selected{ selectHealthy() };
            if (!selected) {
                return std::nullopt;
            }
            auto& backend{ m_backends[*selected] };
            ++backend.active;
            m_cursor = (*selected + 1) % m_backends.size();
            return UpstreamLease{ shared_from_this(), *selected, backend.generation, false };
        }

    private:
        /**
         * @brief Select a healthy backend under the mutex, using rotating load ties.
         */
        auto selectHealthy() -> std::optional<std::size_t> {
            std::optional<std::size_t> selected;
            std::int64_t total{ 0 };
            for (std::size_t offset{ 0 }; offset < m_backends.size(); ++offset) {
                auto const index{ (m_cursor + offset) % m_backends.size() };
                auto& backend{ m_backends[index] };
                if (backend.health != Health::Healthy) {
                    continue;
                }
                if (m_config.strategy == UpstreamStrategy::RoundRobin) {
                    return index;
                }
                if (m_config.strategy == UpstreamStrategy::WeightedRoundRobin) {
                    backend.current += backend.weight;
                    total += backend.weight;
                    if (!selected || backend.current > m_backends[*selected].current) {
                        selected = index;
                    }
                } else if (!selected || backend.active < m_backends[*selected].active) {
                    selected = index;
                }
            }
            if (selected && m_config.strategy == UpstreamStrategy::WeightedRoundRobin) {
                m_backends[*selected].current -= total;
            }
            return selected;
        }
    };

    auto UpstreamLease::Url() const -> url::UrlInfo const& {
        return m_group->m_backends[m_index].url;
    }

    auto UpstreamLease::Success() -> void {
        if (m_group) {
            std::lock_guard const lock{ m_group->m_mutex };
            auto& backend{ m_group->m_backends[m_index] };
            if (m_probe && backend.generation == m_generation && backend.health == UpstreamGroup::Health::Probing) {
                backend.health = UpstreamGroup::Health::Healthy;
                backend.current = 0;
            }
        }
    }

    auto UpstreamLease::Failure() -> void {
        if (m_group && m_group->m_config.failure_cooldown != std::chrono::steady_clock::duration::zero()) {
            std::lock_guard const lock{ m_group->m_mutex };
            auto& backend{ m_group->m_backends[m_index] };
            if (backend.generation == m_generation) {
                ++backend.generation;
                backend.health = UpstreamGroup::Health::Cooling;
                auto const now{ std::chrono::steady_clock::now() };
                backend.retry_at = now + std::min(m_group->m_config.failure_cooldown, std::chrono::steady_clock::time_point::max() - now);
                backend.current = 0;
            }
        }
    }

    auto UpstreamLease::Release() noexcept -> void {
        if (auto group{ std::exchange(m_group, {}) }) {
            std::lock_guard const lock{ group->m_mutex };
            auto& backend{ group->m_backends[m_index] };
            --backend.active;
            if (m_probe && backend.generation == m_generation && backend.health == UpstreamGroup::Health::Probing) {
                // A cancelled probe is immediately eligible for another real request.
                backend.health = UpstreamGroup::Health::Cooling;
            }
        }
    }

} // namespace mhttp::detail
