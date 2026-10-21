/**
 * @file endpoint.cppm
 * @brief Describe a network endpoint without exposing transport-library types.
 */
export module mhttp:core.endpoint;

import std;

export namespace mhttp {

    /**
     * @brief Own an address string and port independent of socket lifetime.
     */
    struct Endpoint {
        std::string address; ///< Owned IP address or bind hostname; IPv6 addresses need no brackets.
        std::uint16_t port{}; ///< TCP port; zero requests dynamic allocation when binding.

        /**
         * @brief Compare endpoint values without resolving or normalizing addresses.
         * @param other Endpoint to compare.
         * @return Whether address bytes and port are equal.
         */
        auto operator==(Endpoint const& other) const -> bool = default;
    };

} // namespace mhttp
