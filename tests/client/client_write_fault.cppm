/**
 * @file client_write_fault.cppm
 * @brief Script transport completions only in the isolated private-client acceptance build.
 */
module mhttp:test.client_write_fault;

import std;
import :core.error;

namespace mhttp::test {

    /**
     * @brief Describe one failed request-head transfer without depending on the transport implementation.
     */
    struct ClientWriteFailure {
        Error error{ Error::ConnectionClosed }; ///< Transport failure reported to the actual client state machine.
        std::size_t bytes_transferred{}; ///< Completed prefix reported by the transfer.
    };

    /**
     * @brief Serialize fault configuration and observations across test and I/O threads.
     * @note The harness imports this partition into a copied transport source only. Production never imports it.
     */
    class ClientWriteFault {
    private:
        inline static std::mutex s_mutex; ///< Protects all script state.
        inline static std::vector<ClientWriteFailure> s_failures; ///< Owned scripted failures in attempt order.
        inline static std::size_t s_calls{}; ///< Number of matching head attempts, including eventual success.

    public:
        /**
         * @brief Replace the script after the preceding scenario has drained.
         * @param failures Owned failed completions; later attempts use the real transport.
         */
        static auto Configure(std::vector<ClientWriteFailure> failures) -> void {
            std::scoped_lock lock{ s_mutex };
            s_failures = std::move(failures);
            s_calls = 0;
        }

        /**
         * @brief Intercept only the dedicated upload head, preserving real connects, bodies, and responses.
         * @param bytes Borrowed current write bytes, inspected only during this call.
         * @return Next scripted failure, or no interception.
         */
        static auto Take(std::string_view bytes) -> std::optional<ClientWriteFailure> {
            if (!bytes.starts_with("POST /retry HTTP/1.1\r\n")) { return std::nullopt; }
            std::scoped_lock lock{ s_mutex };
            auto const index = s_calls++;
            if (index < s_failures.size()) { return s_failures[index]; }
            return std::nullopt;
        }

        /**
         * @brief Observe completed head-attempt accounting without racing the transport strand.
         * @return Number of matching calls since Configure.
         */
        [[nodiscard]] static auto Calls() -> std::size_t {
            std::scoped_lock lock{ s_mutex };
            return s_calls;
        }
    };

} // namespace mhttp::test
