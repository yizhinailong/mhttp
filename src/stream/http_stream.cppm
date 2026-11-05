/**
 * @file http_stream.cppm
 * @brief Wrap the connection's byte stream behind one type (plain TCP until TLS is ported).
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <boost/beast/core/rate_policy.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:http_stream;

import std;
import :config;

namespace mhttp {

    /**
     * @brief Connection byte stream with per-operation timeouts.
     *
     * Satisfies Beast's AsyncReadStream and AsyncWriteStream, so http::async_read and
     * http::async_write accept it directly. httplib's version is a variant over plain and TLS
     * streams; only the plain alternative exists until TLS is ported, and this wrapper is where
     * the variant returns.
     */
    class HttpStream {
    public:
        using PlainStream = beast::basic_stream<tcp, net::any_io_executor, beast::simple_rate_policy>;
        using executor_type = PlainStream::executor_type;

    private:
        PlainStream m_stream; ///< Owned TCP stream with timeout support.

    public:
        /**
         * @brief Take ownership of an accepted or created plain stream.
         * @param stream Stream to own.
         */
        explicit HttpStream(PlainStream&& stream) : m_stream{ std::move(stream) } {}

        /**
         * @brief Return the executor the stream's operations complete on.
         * @return The connection's executor (its strand on the server side).
         */
        auto get_executor() -> executor_type {
            return m_stream.get_executor();
        }

        /**
         * @brief Start an asynchronous read; part of the AsyncReadStream protocol.
         * @param buffers Destination buffers.
         * @param token Completion token.
         * @return Whatever the completion token produces.
         */
        template <typename MutableBufferSequence, typename ReadToken>
        auto async_read_some(MutableBufferSequence const& buffers, ReadToken&& token) {
            return m_stream.async_read_some(buffers, std::forward<ReadToken>(token));
        }

        /**
         * @brief Start an asynchronous write; part of the AsyncWriteStream protocol.
         * @param buffers Source buffers.
         * @param token Completion token.
         * @return Whatever the completion token produces.
         */
        template <typename ConstBufferSequence, typename WriteToken>
        auto async_write_some(ConstBufferSequence const& buffers, WriteToken&& token) {
            return m_stream.async_write_some(buffers, std::forward<WriteToken>(token));
        }

        /**
         * @brief Borrow the underlying socket.
         * @return Socket owned by this stream.
         */
        auto Socket() -> tcp::socket& {
            return m_stream.socket();
        }

        /**
         * @brief Report whether the socket is open.
         * @return Snapshot of the socket's open state.
         */
        [[nodiscard]] auto IsOpen() const -> bool {
            return m_stream.socket().is_open();
        }

        /**
         * @brief Bound the next asynchronous operations by a relative timeout.
         * @param expiry Time allowed before pending operations fail with beast::error::timeout.
         */
        auto ExpiresAfter(net::steady_timer::duration expiry) -> void {
            m_stream.expires_after(expiry);
        }

        /**
         * @brief Remove any pending timeout.
         */
        auto ExpiresNever() -> void {
            m_stream.expires_never();
        }

        /**
         * @brief Connect to one endpoint, bounded by the pending timeout.
         * @param endpoint Server endpoint.
         * @param ec Receives the connect error.
         */
        auto AsyncConnect(
            tcp::endpoint const& endpoint,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await m_stream.async_connect(endpoint, net::redirect_error(net::use_awaitable, ec));
            configureConnected(ec);
        }

        /**
         * @brief Connect to the first reachable endpoint of a resolver result.
         * @tparam EndpointSequence Range of endpoints, such as tcp::resolver::results_type.
         * @param endpoints Candidate endpoints, tried in order.
         * @param ec Receives the error of the last attempt.
         */
        template <typename EndpointSequence>
        auto AsyncConnect(
            EndpointSequence const& endpoints,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await m_stream.async_connect(endpoints, net::redirect_error(net::use_awaitable, ec));
            configureConnected(ec);
        }

        /**
         * @brief Probe an idle connection without consuming data.
         * @return True when the peer has not closed it and no stray bytes are waiting.
         * @note Only call while no asynchronous operation is pending on the stream. httplib also
         *       reported a connection with unread bytes as alive; those bytes belong to no
         *       request and would be parsed as the next response.
         */
        [[nodiscard]] auto IsPeerAlive() -> bool {
            auto& socket{ m_stream.socket() };
            boost::system::error_code ec;
            socket.non_blocking(true, ec);
            if (ec) {
                return false;
            }
            char byte{};
            socket.receive(net::buffer(&byte, 1), net::socket_base::message_peek, ec);
            boost::system::error_code restore_ec;
            socket.non_blocking(false, restore_ec);
            return ec == net::error::would_block || ec == net::error::try_again;
        }

        /**
         * @brief Cap the throughput of later reads and writes.
         * @param read_bytes_per_second Read limit; 0 means unlimited.
         * @param write_bytes_per_second Write limit; 0 means unlimited.
         */
        auto SetRateLimits(
            std::uint64_t read_bytes_per_second,
            std::uint64_t write_bytes_per_second
        ) -> void {
            auto const to_limit{ [](std::uint64_t value) {
                return value == 0 ? (std::numeric_limits<std::size_t>::max)() : static_cast<std::size_t>(value);
            } };
            m_stream.rate_policy().read_limit(to_limit(read_bytes_per_second));
            m_stream.rate_policy().write_limit(to_limit(write_bytes_per_second));
        }

        /**
         * @brief Release the socket immediately.
         * @param ec Receives the last error; failures are not actionable at this point.
         * @note Shuts down only the send direction before closing. Shutting down the receive
         *       direction makes Windows answer later peer data (such as an unread request body)
         *       with RST, which discards a response the peer has not read yet (WSAECONNRESET).
         */
        auto Close(boost::system::error_code& ec) -> void {
            m_stream.socket().shutdown(net::socket_base::shutdown_send, ec);
            m_stream.socket().close(ec);
        }

        /**
         * @brief Finish the connection gracefully so the peer sees a clean EOF.
         * @note TLS will send close_notify here first; for plain TCP this equals Close().
         */
        auto AsyncClose() -> net::awaitable<void> {
            boost::system::error_code ec;
            Close(ec);
            co_return;
        }

    private:
        /**
         * @brief Apply client socket options after a successful connect.
         * @param ec Connect result; options are skipped on failure.
         */
        auto configureConnected(boost::system::error_code const& ec) -> void {
            if (ec) {
                return;
            }
            boost::system::error_code option_ec;
            m_stream.socket().set_option(tcp::no_delay{ true }, option_ec);
            m_stream.socket().set_option(net::socket_base::keep_alive{ true }, option_ec);
        }
    };

} // namespace mhttp
