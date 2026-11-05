/**
 * @file websocket_conn_impl.cppm
 * @brief Drive an upgraded server connection while preserving prefetched frame bytes.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <boost/beast/core/buffered_read_stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/rate_policy.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/beast/websocket/teardown.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:websocket_conn_impl;

import std;
import :config;
import :error;
import :logging;
import :async_event;
import :strand_mutex;
import :request;
import :http_stream;
import :websocket_message;
import :websocket_conn;

namespace mhttp::detail {

    /**
     * @brief Adapt Beast's buffered stream for WebSocket teardown through ADL.
     *
     * The parsed HTTP header is passed directly to async_accept(), so large permitted headers
     * do not pass through Beast's smaller internal handshake buffer. Bytes beyond the HTTP
     * header stay in the next layer and are consumed by the first frame read.
     */
    class WebSocketTransport
        : public beast::buffered_read_stream<HttpStream::PlainStream, beast::flat_buffer> {
    private:
        using BufferedStream = beast::buffered_read_stream<HttpStream::PlainStream, beast::flat_buffer>;

    public:
        /**
         * @brief Take over the HTTP socket and its unread bytes without copying either.
         */
        WebSocketTransport(
            HttpStream&& stream,
            beast::flat_buffer&& buffered
        ) : BufferedStream{ std::move(stream).Release() } {
            buffer() = std::move(buffered);
        }
    };

    /**
     * @brief Forward synchronous WebSocket teardown to the underlying TCP stream.
     */
    auto teardown(
        beast::role_type role,
        WebSocketTransport& stream,
        boost::system::error_code& ec
    ) -> void {
        using beast::websocket::teardown;
        teardown(role, stream.next_layer(), ec);
    }

    /**
     * @brief Forward asynchronous WebSocket teardown to the underlying TCP stream.
     */
    template <typename Handler>
    auto async_teardown(
        beast::role_type role,
        WebSocketTransport& stream,
        Handler&& handler
    ) -> void {
        using beast::websocket::async_teardown;
        async_teardown(role, stream.next_layer(), std::forward<Handler>(handler));
    }

    /**
     * @brief Server-owned WebSocket receive loop with strand-serialized, queued writes.
     */
    class WebSocketConnImpl final : public WebSocketConn {
    private:
        net::any_io_executor m_executor;                         ///< Accepted connection strand.
        Request m_request;                                       ///< Handshake request and user state.
        beast::websocket::stream<WebSocketTransport> m_stream;   ///< Upgraded transport.
        WsHandlerEntry m_handlers;                               ///< Matched route callbacks.
        std::shared_ptr<LoggerHolder> m_logger;                  ///< Aliased server logger; keeps its owner alive.
        StrandMutex m_write_mutex;                               ///< Backpressure for messages and control frames.
        util::AsyncEvent m_write_done;                           ///< Wakes the receive loop as writes drain.
        std::atomic<bool> m_open{ false };                       ///< Thread-safe public state snapshot.
        bool m_aborted{ false };                                 ///< Terminal state; strand only.
        bool m_closing{ false };                                 ///< A graceful close is in progress; strand only.
        std::size_t m_pending_writes{ 0 };                       ///< Active and queued write operations; strand only.
        std::chrono::steady_clock::duration m_handshake_timeout; ///< Upgrade response deadline.
        std::uint64_t m_message_limit;                           ///< Maximum received message size.

        /**
         * @brief Count an operation until its coroutine leaves the connection strand.
         */
        struct WriteGuard {
            WebSocketConnImpl& owner; ///< Owning connection.

            explicit WriteGuard(WebSocketConnImpl& connection) : owner{ connection } {
                ++owner.m_pending_writes;
            }

            ~WriteGuard() {
                --owner.m_pending_writes;
                owner.m_write_done.NotifyAll();
            }
        };

    public:
        /**
         * @brief Transfer an HTTP connection into a WebSocket session.
         */
        WebSocketConnImpl(
            HttpStream&& stream,
            beast::flat_buffer&& buffered,
            Request&& request,
            WsHandlerEntry handlers,
            std::shared_ptr<LoggerHolder> logger,
            std::chrono::steady_clock::duration handshake_timeout,
            std::uint64_t message_limit
        ) : m_executor{ stream.get_executor() }, m_request{ std::move(request) }, m_stream{ std::move(stream), std::move(buffered) }, m_handlers{ std::move(handlers) }, m_logger{ std::move(logger) }, m_write_mutex{ m_executor }, m_write_done{ m_executor }, m_handshake_timeout{ handshake_timeout }, m_message_limit{ message_limit } {}

        auto Executor() const noexcept -> net::any_io_executor override { return m_executor; }

        auto IsOpen() const noexcept -> bool override { return m_open.load(); }

        auto HttpRequest() -> Request& override { return m_request; }

        auto HttpRequest() const -> Request const& override { return m_request; }

        auto AsyncSend(
            WebSocketMessage const& message,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await writeOperation([&]() -> net::awaitable<void> {
                m_stream.binary(message.IsBinary());
                co_await m_stream.async_write(net::buffer(message.View()), net::redirect_error(net::use_awaitable, ec));
            },
                                    ec);
        }

        auto Send(WebSocketMessage message) -> std::future<boost::system::error_code> override {
            return net::co_spawn(m_executor, [self = shared_from_this(), message = std::move(message)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await self->AsyncSend(message, ec);
                co_return ec; }, net::use_future);
        }

        auto AsyncPing(
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await control(true, payload, ec);
        }

        auto Ping(std::string payload = {}) -> std::future<boost::system::error_code> override {
            return net::co_spawn(m_executor, [self = shared_from_this(), payload = std::move(payload)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await self->AsyncPing(payload, ec);
                co_return ec; }, net::use_future);
        }

        auto AsyncPong(
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await control(false, payload, ec);
        }

        auto Pong(std::string payload = {}) -> std::future<boost::system::error_code> override {
            return net::co_spawn(m_executor, [self = shared_from_this(), payload = std::move(payload)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await self->AsyncPong(payload, ec);
                co_return ec; }, net::use_future);
        }

        auto AsyncClose(
            std::string_view reason,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            if (reason.size() > 123) {
                ec = net::error::message_size;
                co_return;
            }
            beast::websocket::close_reason close_reason{ beast::websocket::close_code::normal };
            close_reason.reason.assign(reason.data(), reason.size());
            co_await closeOperation(close_reason, ec);
        }

        auto Close(
            std::string reason = "normal"
        ) -> std::future<boost::system::error_code> override {
            return net::co_spawn(m_executor, [self = shared_from_this(), reason = std::move(reason)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await self->AsyncClose(reason, ec);
                co_return ec; }, net::use_future);
        }

        auto AsyncAbort() -> net::awaitable<void> override {
            co_await net::co_spawn(m_executor, [this, self = shared_from_this()]() -> net::awaitable<void> {
                AbortNow();
                co_return; }, net::use_awaitable);
        }

        auto Abort() -> std::future<void> override {
            return net::co_spawn(m_executor, [this, self = shared_from_this()]() -> net::awaitable<void> {
                AbortNow();
                co_return; }, net::use_future);
        }

        /**
         * @brief Perform the upgrade, receive messages, drain writes, and call close exactly once.
         * @note Must run on Executor(); the session holds this object throughout the call.
         */
        auto Run() -> net::awaitable<void> {
            if (m_aborted) {
                co_return;
            }
            beast::get_lowest_layer(m_stream).expires_never();
            beast::websocket::stream_base::timeout options{};
            options.handshake_timeout = m_handshake_timeout;
            options.idle_timeout = beast::websocket::stream_base::none();
            options.keep_alive_pings = false;
            m_stream.set_option(options);
            // Beast treats zero as unlimited; preserve SetBodyLimit(0)'s empty-only meaning.
            m_stream.read_message_max(std::max<std::uint64_t>(m_message_limit, 1));
            m_stream.set_option(beast::websocket::stream_base::decorator([](beast::websocket::response_type& response) {
                response.set(Field::server, std::string{ BOOST_BEAST_VERSION_STRING } + " mhttp");
            }));

            boost::system::error_code ec;
            co_await m_stream.async_accept(get_impl(m_request).Message(), net::redirect_error(net::use_awaitable, ec));
            if (ec || m_aborted) {
                m_logger->GetLogger()->debug("websocket handshake failed for {}: {}", m_request.Target(), ec.message());
                AbortNow();
                co_return;
            }
            options.handshake_timeout = std::chrono::seconds{ 5 };
            m_stream.set_option(options);
            m_open.store(true);
            try {
                co_await m_handlers.open(weak_from_this());
            } catch (std::exception const& e) {
                m_logger->GetLogger()->error("websocket open handler failed: {}", e.what());
                AbortNow();
            } catch (...) {
                m_logger->GetLogger()->error("websocket open handler failed with an unknown exception");
                AbortNow();
            }

            while (m_open.load()) {
                WebSocketMessage message;
                auto buffer{ net::dynamic_buffer(message.Data()) };
                co_await m_stream.async_read(buffer, net::redirect_error(net::use_awaitable, ec));
                if (ec || !m_open.load()) {
                    break;
                }
                if (message.Data().size() > m_message_limit) {
                    co_await closeOperation(beast::websocket::close_reason{ beast::websocket::close_code::too_big }, ec);
                    break;
                }
                message.SetBinary(m_stream.got_binary());
                try {
                    co_await m_handlers.message(weak_from_this(), std::move(message));
                } catch (std::exception const& e) {
                    m_logger->GetLogger()->error("websocket message handler failed: {}", e.what());
                    AbortNow();
                } catch (...) {
                    m_logger->GetLogger()->error("websocket message handler failed with an unknown exception");
                    AbortNow();
                }
            }
            if (!m_closing) {
                AbortNow();
            }
            while (m_pending_writes > 0) {
                co_await m_write_done.Wait();
            }
            try {
                co_await m_handlers.close(weak_from_this());
            } catch (std::exception const& e) {
                m_logger->GetLogger()->error("websocket close handler failed: {}", e.what());
            } catch (...) {
                m_logger->GetLogger()->error("websocket close handler failed with an unknown exception");
            }
        }

        /**
         * @brief Interrupt the handshake, pending reads, and queued writes; connection strand only.
         */
        auto AbortNow() -> void {
            m_open.store(false);
            if (!std::exchange(m_aborted, true)) {
                beast::get_lowest_layer(m_stream).close();
                m_write_mutex.Close();
            }
        }

    private:
        /**
         * @brief Serialize a close handshake with application writes and cancel the transport.
         */
        auto closeOperation(
            beast::websocket::close_reason const& reason,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await writeOperation([&]() -> net::awaitable<void> {
                m_closing = true;
                m_open.store(false);
                co_await m_stream.async_close(reason, net::redirect_error(net::use_awaitable, ec));
                m_closing = false;
                AbortNow();
            },
                                    ec);
        }

        /**
         * @brief Queue a write on the connection strand and keep the connection alive until it ends.
         */
        template <typename Body>
        auto writeOperation(Body body, boost::system::error_code& ec) -> net::awaitable<void> {
            co_await net::co_spawn(m_executor, [this, self = shared_from_this(), body = std::move(body), &ec]() mutable -> net::awaitable<void> {
                WriteGuard const pending{ *this };
                ec = {};
                if (!m_open.load()) {
                    ec = Error::NotInitialized;
                    co_return;
                }
                if ((ec = co_await m_write_mutex.Lock())) {
                    co_return;
                }
                StrandLock const lock{ m_write_mutex };
                if (!m_open.load()) {
                    ec = net::error::operation_aborted;
                    co_return;
                }
                co_await body();
                if (ec) {
                    AbortNow();
                } }, net::use_awaitable);
        }

        /**
         * @brief Validate a control payload and queue a ping or unsolicited pong.
         */
        auto control(
            bool ping,
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            if (payload.size() > 125) {
                ec = net::error::message_size;
                co_return;
            }
            co_await writeOperation([&]() -> net::awaitable<void> {
                beast::websocket::ping_data data;
                data.assign(payload.data(), payload.size());
                if (ping) {
                    co_await m_stream.async_ping(data, net::redirect_error(net::use_awaitable, ec));
                } else {
                    co_await m_stream.async_pong(data, net::redirect_error(net::use_awaitable, ec));
                }
            },
                                    ec);
        }
    };

} // namespace mhttp::detail
