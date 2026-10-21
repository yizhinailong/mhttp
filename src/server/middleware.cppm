/**
 * @file middleware.cppm
 * @brief Own independently ordered global Before and After callbacks across lazy Tasks.
 */
export module mhttp:server.middleware;

import std;
export import :server.handler;

export namespace mhttp::detail {

    /**
     * @brief Own an asynchronous boolean routing decision without exposing the callback's concrete type.
     * @internal
     */
    using HttpDecision = std::function<Task<bool>(server::Request&, server::Response&)>;

    /**
     * @brief Keep both aspect phases bound to the same stable owned object.
     * @internal
     */
    struct HttpMiddleware {
        HttpDecision before; ///< Before decision; an absent aspect member returns true.
        HttpDecision after; ///< After decision; an absent aspect member returns true.
    };

    /**
     * @brief Retain aspect ownership before initial suspension and delegate one optional member.
     * @tparam BEFORE Select Before when true and After otherwise.
     * @tparam A Owned aspect type.
     * @param aspect Stable owned aspect, retained through completion or cancellation.
     * @param request Borrowed through the entire callback chain.
     * @param response Borrowed for the same lifetime.
     * @return Member decision, or true for an absent member.
     * @internal
     */
    template <bool BEFORE, typename A>
    auto invoke_owned_http_aspect(
        std::shared_ptr<A> aspect,
        server::Request& request,
        server::Response& response
    ) -> Task<bool> {
        co_return co_await invoke_http_aspect<BEFORE>(*aspect, request, response);
    }

    /**
     * @brief Split an owned aspect into independent Before/After entry points sharing stable state.
     * @tparam A Eligible synchronous or asynchronous aspect type.
     * @param aspect Aspect copied or moved before publication to the router.
     * @return Copyable adapters whose issued Tasks own their state before initial suspension.
     * @note Reference captures remain borrowed; concurrent requests may call the same aspect object.
     * @internal
     */
    template <typename A>
        requires HttpAspect<std::decay_t<A>>
    auto make_http_middleware(A&& aspect) -> HttpMiddleware {
        auto state = std::make_shared<std::decay_t<A>>(std::forward<A>(aspect));
        return {
            [state](server::Request& request, server::Response& response) {
                return invoke_owned_http_aspect<true>(state, request, response);
            },
            [state](server::Request& request, server::Response& response) {
                return invoke_owned_http_aspect<false>(state, request, response);
            },
        };
    }

} // namespace mhttp::detail
