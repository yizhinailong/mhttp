/**
 * @file routing.cppm
 * @brief Carry prepared route ownership between header matching and buffered request execution.
 */
export module mhttp:server.routing;

import std;
export import :server.route_tree;
export import :server.hooks;

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
        bool lazy{}; ///< Selected route reads its body on demand inside middleware/aspects/handler.
        bool handled{}; ///< Pre-routing produced the response; skip matching, body intake, and route aspects.
        std::exception_ptr failure; ///< Preparation failure deferred to processing without reading the body.
    };

    /**
     * @brief Tell a session whether a finalized response can be sent and whether persistence is allowed.
     * @note Callback exceptions always require close, even if the exception handler changes status.
     *       A failed post-routing hook forbids writing a partly finalized response. Cancellation propagates.
     * @internal
     */
    struct RoutingOutcome {
        bool close{}; ///< Close after the response, irrespective of application Connection headers.
        bool send_response{ true }; ///< False when post-routing failed; close without attempting a response.
        std::exception_ptr failure; ///< Last contained callback failure, available for logging.
    };

} // namespace mhttp::detail
