/**
 * @file deadlines.cppm
 * @brief Compute monotonic client deadline budgets without resetting an overall request deadline.
 */
module mhttp:client.impl.deadlines;

import std;
import :client.options;

namespace mhttp::impl {

    /**
     * @brief Own one Send's deadline policy across every connection attempt and redirect hop.
     * @note Phase deadlines are created once per phase; Step budgets are renewed per transport operation.
     */
    class ClientDeadlines {
    private:
        using Clock = std::chrono::steady_clock; ///< Deadline clock shared with transport timers.
        client::TimeoutPolicy m_policy; ///< Immutable policy for this Send.
        Clock::duration m_timeout; ///< Overall or Step budget.
        Clock::time_point m_overall_until; ///< Fixed end of the Send, including redirects and retries.

    public:
        /**
         * @brief Start the request budget after preflight validation and single-flight admission.
         * @param options Validated immutable configuration, copied rather than retained.
         */
        explicit ClientDeadlines(client::ClientOptions const& options) :
            m_policy{ options.timeout_policy }, m_timeout{ options.timeout },
            m_overall_until{ Clock::now() + m_timeout } {}

        /**
         * @brief Start a phase deadline or select the fixed overall deadline.
         * @param timeout Complete phase budget when using Phase policy.
         * @return Fixed deadline for Phase/Overall, absent for Step/Never.
         */
        [[nodiscard]] auto Until(std::chrono::milliseconds timeout) const -> std::optional<Clock::time_point> {
            if (m_policy == client::TimeoutPolicy::Phase) { return Clock::now() + timeout; }
            if (m_policy == client::TimeoutPolicy::Overall) { return m_overall_until; }
            return {};
        }

        /**
         * @brief Compute the remaining budget immediately before starting the next operation.
         * @param until Fixed phase/overall deadline from Until, absent for Step/Never.
         * @return Remaining or renewed duration; absent disables timing, nonpositive expires immediately.
         */
        [[nodiscard]] auto Remaining(
            std::optional<Clock::time_point> until
        ) const -> std::optional<Clock::duration> {
            if (m_policy == client::TimeoutPolicy::Never) { return {}; }
            if (m_policy == client::TimeoutPolicy::Step) { return m_timeout; }
            return *until - Clock::now();
        }
    };

} // namespace mhttp::impl
