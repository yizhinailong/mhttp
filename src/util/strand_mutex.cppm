/**
 * @file strand_mutex.cppm
 * @brief Serialize coroutine operations on one strand with a bounded channel.
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:strand_mutex;

import std;
import :config;

namespace mhttp::detail {

    /**
     * @brief FIFO coroutine mutex; every call must run on its executor's strand.
     */
    class StrandMutex {
    private:
        net::experimental::channel<void(boost::system::error_code)> m_channel; ///< Owner token.

    public:
        /**
         * @brief Create an unlocked mutex.
         * @param executor Strand shared by all users.
         */
        explicit StrandMutex(net::any_io_executor executor) : m_channel{ std::move(executor), 1 } {}

        /**
         * @brief Wait for ownership.
         * @return Error if closed or cancelled while waiting.
         */
        auto Lock() -> net::awaitable<boost::system::error_code> {
            boost::system::error_code ec;
            co_await m_channel.async_send(boost::system::error_code{}, net::redirect_error(net::use_awaitable, ec));
            co_return ec;
        }

        /**
         * @brief Release ownership and admit the next waiter.
         */
        auto Unlock() -> void {
            m_channel.try_receive([](boost::system::error_code) {});
        }

        /**
         * @brief Cancel every queued lock and reject future locks.
         */
        auto Close() -> void {
            m_channel.close();
        }
    };

    /**
     * @brief Release an acquired StrandMutex at scope exit.
     */
    class StrandLock {
    private:
        StrandMutex* m_mutex; ///< Owned mutex.

    public:
        /**
         * @brief Adopt a mutex already acquired by the caller.
         */
        explicit StrandLock(StrandMutex& mutex) : m_mutex{ &mutex } {}

        StrandLock(StrandLock const&) = delete;
        auto operator=(StrandLock const&) -> StrandLock& = delete;

        /**
         * @brief Release ownership.
         */
        ~StrandLock() { m_mutex->Unlock(); }
    };

} // namespace mhttp::detail
