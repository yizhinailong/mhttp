/**
 * @file when_all.cppm
 * @brief Await a batch of coroutines concurrently and collect their results in order.
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/this_coro.hpp>

export module mhttp:when_all;

import std;
import :config;

export namespace mhttp::util {

    /**
     * @brief Run awaitables concurrently on the caller's executor and wait for all of them.
     * @tparam T Result type of each awaitable; must be default-constructible.
     * @param ops Awaitables to run; consumed by the call.
     * @return Results indexed like `ops`, independent of completion order.
     * @throws Rethrows the first exception, by operation index, after every operation finished.
     */
    template <typename T>
    auto when_all(std::vector<net::awaitable<T>> ops) -> net::awaitable<std::vector<T>> {
        if (ops.empty()) {
            co_return std::vector<T>{};
        }
        auto executor{ co_await net::this_coro::executor };

        using Operation = decltype(net::co_spawn(executor, std::move(ops.front()), net::deferred));
        std::vector<Operation> spawned;
        spawned.reserve(ops.size());
        for (auto& op : ops) {
            spawned.push_back(net::co_spawn(executor, std::move(op), net::deferred));
        }

        auto [order, exceptions, values] = co_await net::experimental::make_parallel_group(std::move(spawned))
                                               .async_wait(net::experimental::wait_for_all(), net::deferred);
        for (auto const& exception : exceptions) {
            if (exception) {
                std::rethrow_exception(exception);
            }
        }
        co_return std::move(values);
    }

    /**
     * @brief Run void awaitables concurrently on the caller's executor and wait for all of them.
     * @param ops Awaitables to run; consumed by the call.
     * @throws Rethrows the first exception, by operation index, after every operation finished.
     */
    auto when_all(std::vector<net::awaitable<void>> ops) -> net::awaitable<void> {
        if (ops.empty()) {
            co_return;
        }
        auto executor{ co_await net::this_coro::executor };

        using Operation = decltype(net::co_spawn(executor, std::move(ops.front()), net::deferred));
        std::vector<Operation> spawned;
        spawned.reserve(ops.size());
        for (auto& op : ops) {
            spawned.push_back(net::co_spawn(executor, std::move(op), net::deferred));
        }

        auto [order, exceptions] = co_await net::experimental::make_parallel_group(std::move(spawned))
                                       .async_wait(net::experimental::wait_for_all(), net::deferred);
        for (auto const& exception : exceptions) {
            if (exception) {
                std::rethrow_exception(exception);
            }
        }
    }

} // namespace mhttp::util
