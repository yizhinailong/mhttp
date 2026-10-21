/**
 * @file handler.cppm
 * @brief Adapt owned synchronous and Task handlers with ordered synchronous or asynchronous aspects.
 */
export module mhttp:server.handler;

import std;
export import :core.task;
export import :server.request;
export import :server.response;

export namespace mhttp::detail {

    /**
     * @brief Accept a synchronous void handler or the library's coroutine handler signature.
     * @tparam F Callable invoked as an lvalue with borrowed request and response references.
     * @internal
     */
    template <typename F>
    concept HttpHandlerFunction = std::invocable<F&, server::Request&, server::Response&>
        && (std::same_as<std::invoke_result_t<F&, server::Request&, server::Response&>, void>
            || std::same_as<std::invoke_result_t<F&, server::Request&, server::Response&>, Task<void>>);

    /**
     * @brief Accept an aspect decision returned directly or through a Task.
     * @tparam T Exact result type.
     * @internal
     */
    template <typename T>
    concept HttpAspectResult = std::same_as<T, bool> || std::same_as<T, Task<bool>>;

    /**
     * @brief Require at least one eligible Before/After member and validate every callable member's result.
     * @tparam A Owned aspect type.
     * @internal
     */
    template <typename A>
    concept HttpAspect = (requires(A& aspect, server::Request& request, server::Response& response) {
        aspect.Before(request, response);
    } || requires(A& aspect, server::Request& request, server::Response& response) {
        aspect.After(request, response);
    }) && (!requires(A& aspect, server::Request& request, server::Response& response) {
        aspect.Before(request, response);
    } || requires(A& aspect, server::Request& request, server::Response& response) {
        { aspect.Before(request, response) } -> HttpAspectResult;
    }) && (!requires(A& aspect, server::Request& request, server::Response& response) {
        aspect.After(request, response);
    } || requires(A& aspect, server::Request& request, server::Response& response) {
        { aspect.After(request, response) } -> HttpAspectResult;
    });

    /**
     * @brief Own the type-erased entry point used by routing and per-request execution.
     * @internal
     */
    using HttpHandler = std::function<Task<void>(server::Request&, server::Response&)>;

    /**
     * @brief Keep callable and aspect addresses stable until every issued handler Task is destroyed.
     * @tparam F Owned decayed handler type.
     * @tparam Aspects Owned decayed aspect types in registration order.
     * @internal
     */
    template <typename F, typename... Aspects>
    struct HttpHandlerState {
        F handler; ///< Owned callable; shared business state requires application synchronization.
        std::tuple<Aspects...> aspects; ///< Owned route aspect sequence.

        /**
         * @brief Construct owned callback state directly, including move-only callable or aspect types.
         * @tparam InputF Forwarded callable argument type.
         * @tparam InputAspects Forwarded aspect argument types.
         * @param function Callable copied or moved into storage.
         * @param values Aspects copied or moved in registration order.
         */
        template <typename InputF, typename... InputAspects>
        HttpHandlerState(InputF&& function, InputAspects&&... values) :
            handler{ std::forward<InputF>(function) }, aspects{ std::forward<InputAspects>(values)... } {}
    };

    /**
     * @brief Invoke an optional aspect member and await its decision when asynchronous.
     * @tparam BEFORE Select Before when true and After otherwise.
     * @tparam A Aspect type with an optional eligible member.
     * @param aspect Borrowed from the outer Task's owned callback state.
     * @param request Request borrowed until the entire handler Task completes.
     * @param response Response borrowed for the same lifetime.
     * @return True for an absent member, otherwise its decision.
     * @internal
     */
    template <bool BEFORE, typename A>
    auto invoke_http_aspect(
        A& aspect,
        server::Request& request,
        server::Response& response
    ) -> Task<bool> {
        if constexpr (BEFORE) {
            if constexpr (requires { aspect.Before(request, response); }) {
                if constexpr (std::same_as<decltype(aspect.Before(request, response)), Task<bool>>) {
                    co_return co_await aspect.Before(request, response);
                } else { co_return aspect.Before(request, response); }
            }
        } else {
            if constexpr (requires { aspect.After(request, response); }) {
                if constexpr (std::same_as<decltype(aspect.After(request, response)), Task<bool>>) {
                    co_return co_await aspect.After(request, response);
                } else { co_return aspect.After(request, response); }
            }
        }
        co_return true;
    }

    /**
     * @brief Evaluate aspect members in registration order until the first false decision.
     * @tparam BEFORE Select Before or After members.
     * @tparam INDEX Next tuple element.
     * @tparam Tuple Owned aspect tuple type.
     * @param aspects Tuple borrowed from the outer Task's owned callback state.
     * @param request Request borrowed through completion.
     * @param response Response borrowed through completion.
     * @return Whether every eligible member returned true.
     * @internal
     */
    template <bool BEFORE, std::size_t INDEX, typename Tuple>
    auto invoke_http_aspects(
        Tuple& aspects,
        server::Request& request,
        server::Response& response
    ) -> Task<bool> {
        if constexpr (INDEX == std::tuple_size_v<Tuple>) { co_return true; }
        else {
            if (!co_await invoke_http_aspect<BEFORE>(std::get<INDEX>(aspects), request, response)) { co_return false; }
            co_return co_await invoke_http_aspects<BEFORE, INDEX + 1>(aspects, request, response);
        }
    }

    /**
     * @brief Own callbacks in the coroutine frame before initial suspension, independently of the adapter.
     * @tparam F Handler type.
     * @tparam Aspects Aspect types.
     * @param state Owned stable callable and aspect state.
     * @param request Borrowed until this Task is destroyed after completion or cancellation.
     * @param response Borrowed for the same lifetime.
     * @note Before false skips the remaining Before chain and handler, then runs After in registration
     *       order. After false skips remaining After members. Exceptions propagate and skip later stages.
     * @internal
     */
    template <typename F, typename... Aspects>
    auto invoke_http_handler(
        std::shared_ptr<HttpHandlerState<F, Aspects...>> state,
        server::Request& request,
        server::Response& response
    ) -> Task<void> {
        if (co_await invoke_http_aspects<true, 0>(state->aspects, request, response)) {
            if constexpr (std::same_as<std::invoke_result_t<F&, server::Request&, server::Response&>, Task<void>>) {
                co_await std::invoke(state->handler, request, response);
            } else { std::invoke(state->handler, request, response); }
        }
        static_cast<void>(co_await invoke_http_aspects<false, 0>(state->aspects, request, response));
    }

    /**
     * @brief Own a synchronous or Task handler and optional ordered aspects behind a copyable adapter.
     * @tparam F Eligible callable type, decayed and copied or moved into stable owned storage.
     * @tparam Aspects Eligible aspect types, decayed and copied or moved in registration order.
     * @param handler Synchronous void or Task<void> callable accepting Request& and Response&.
     * @param aspects Optional Before/After objects returning bool or Task<bool>.
     * @return Non-coroutine closure whose returned Tasks retain callback state before initial suspension.
     * @note Captured references still require their referents to outlive all issued Tasks. Each invocation
     *       borrows request/response until completion. Application callback state may be invoked concurrently.
     * @internal
     */
    template <typename F, typename... Aspects>
        requires HttpHandlerFunction<std::decay_t<F>> && (HttpAspect<std::decay_t<Aspects>> && ...)
    auto make_http_handler(F&& handler, Aspects&&... aspects) -> HttpHandler {
        auto state = std::make_shared<HttpHandlerState<std::decay_t<F>, std::decay_t<Aspects>...>>(
            std::forward<F>(handler), std::forward<Aspects>(aspects)...);
        return [state = std::move(state)](server::Request& request, server::Response& response) {
            return invoke_http_handler(state, request, response);
        };
    }

} // namespace mhttp::detail
