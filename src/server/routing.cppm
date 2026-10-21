/**
 * @file routing.cppm
 * @brief Carry prepared route ownership between header matching and buffered request execution.
 */
export module mhttp:server.routing;

import std;
export import :server.route_tree;
export import :server.handler;

export namespace mhttp::detail {

    /**
     * @brief Own a selected callback and configuration lease until the session has finished using them.
     * @note Prepare populates Request path captures before returning. Keep this object through the
     *       response write; releasing the final lease permits configuration changes when unfrozen.
     * @internal
     */
    struct PreparedRoute {
        RouteMatch match; ///< Method/path outcome; captures have been transferred into Request.
        std::shared_ptr<HttpHandler const> handler; ///< Stable selected callback, absent for 404/405.
        std::shared_ptr<void> lifetime; ///< Type-erased configuration lease, also owning the router state.
    };

} // namespace mhttp::detail
