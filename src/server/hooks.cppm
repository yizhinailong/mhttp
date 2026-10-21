/**
 * @file hooks.cppm
 * @brief Adapt owned synchronous and asynchronous pre-routing decisions.
 */
export module mhttp:server.hooks;

import std;
export import :server.middleware;

export namespace mhttp::detail {

    /**
     * @brief Accept a boolean pre-routing decision directly or through Task<bool>.
     * @tparam F Callable invoked as an lvalue with borrowed request and response.
     * @internal
     */
    template <typename F>
    concept HttpDecisionFunction = std::invocable<F&, server::Request&, server::Response&>
        && HttpAspectResult<std::invoke_result_t<F&, server::Request&, server::Response&>>;

    /**
     * @brief Own a decision callable in the coroutine frame before initial suspension.
     * @tparam F Eligible callback type.
     * @param function Owned stable callback.
     * @param request Borrowed through the entire hook invocation.
     * @param response Borrowed for the same lifetime.
     * @return Callback decision; true means pre-routing handled the request.
     * @internal
     */
    template <typename F>
    auto invoke_http_decision(
        std::shared_ptr<F> function,
        server::Request& request,
        server::Response& response
    ) -> Task<bool> {
        if constexpr (std::same_as<std::invoke_result_t<F&, server::Request&, server::Response&>, Task<bool>>) {
            co_return co_await std::invoke(*function, request, response);
        } else { co_return std::invoke(*function, request, response); }
    }

    /**
     * @brief Own a sync/Task decision without tying returned Tasks to the adapter object's lifetime.
     * @tparam F Eligible decision callback type.
     * @param function Callback copied or moved into stable storage.
     * @return Adapter issuing owned lazy Tasks.
     * @internal
     */
    template <typename F>
        requires HttpDecisionFunction<std::decay_t<F>>
    auto make_http_decision(F&& function) -> HttpDecision {
        auto state = std::make_shared<std::decay_t<F>>(std::forward<F>(function));
        return [state = std::move(state)](server::Request& request, server::Response& response) {
            return invoke_http_decision(state, request, response);
        };
    }

} // namespace mhttp::detail
