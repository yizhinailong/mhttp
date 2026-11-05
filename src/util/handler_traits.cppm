/**
 * @file handler_traits.cppm
 * @brief Adapt synchronous and coroutine callables to one awaitable calling convention.
 */
module;

#include <boost/asio/awaitable.hpp>

export module mhttp:handler_traits;

import std;
import :config;

namespace mhttp::detail {

    /**
     * @brief Detect net::awaitable specializations.
     * @tparam T Type to test.
     */
    template <typename T>
    inline constexpr bool is_awaitable_v = false;

    template <typename T, typename Executor>
    inline constexpr bool is_awaitable_v<net::awaitable<T, Executor>> = true;

    /**
     * @brief Return an already completed awaitable.
     * @return Awaitable that finishes immediately.
     */
    auto make_ready_awaitable() -> net::awaitable<void> {
        co_return;
    }

    /**
     * @brief Return an already completed awaitable holding a value.
     * @tparam T Value type.
     * @param value Value to deliver.
     * @return Awaitable that finishes immediately with `value`.
     */
    template <typename T>
    auto make_ready_awaitable(T value) -> net::awaitable<T> {
        co_return value;
    }

    /**
     * @brief Wrap a callable so it always returns an awaitable.
     *
     * Coroutine callables are returned unchanged. Synchronous callables run at call time and
     * return a ready awaitable; the call deliberately is not moved into a coroutine body,
     * because a lazy coroutine would only start at co_await, after by-value arguments died.
     *
     * @tparam Args Argument types the callable is invoked with.
     * @tparam Func Callable type.
     * @param handler Callable to adapt.
     * @return Callable returning net::awaitable.
     */
    template <typename... Args, typename Func>
    auto make_coro_handler(Func&& handler) {
        using ReturnType = std::invoke_result_t<std::decay_t<Func>&, Args...>;
        if constexpr (is_awaitable_v<ReturnType>) {
            return std::forward<Func>(handler);
        } else {
            return [handler = std::forward<Func>(handler)](Args... args) mutable -> net::awaitable<ReturnType> {
                if constexpr (std::is_void_v<ReturnType>) {
                    std::invoke(handler, std::forward<Args>(args)...);
                    return make_ready_awaitable();
                } else {
                    return make_ready_awaitable(std::invoke(handler, std::forward<Args>(args)...));
                }
            };
        }
    }

} // namespace mhttp::detail
