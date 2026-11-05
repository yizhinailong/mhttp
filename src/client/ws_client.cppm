/**
 * @file ws_client.cppm
 * @brief Provide WsClient: an asynchronous WebSocket client with queued writes.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <boost/beast/core/rate_policy.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:ws_client;

import std;
import :config;
import :error;
import :logging;
import :handler_traits;
import :url;
import :headers;
import :websocket_message;
import :strand_mutex;

namespace mhttp::client {

    class WsClientImpl;

} // namespace mhttp::client

export namespace mhttp::client {

    /**
     * @brief Asynchronous WebSocket client (port of httplib::client::ws_client).
     *
     * Every operation runs on the client's own strand and may be started from any executor.
     * Writes (messages, pings, pongs, close) are queued in call order, so several coroutines
     * may send concurrently; reads must not overlap (a second read fails with
     * Error::OperationInProgress). Permessage-deflate and TLS are not ported: a wss client
     * fails to connect with net::error::operation_not_supported.
     */
    class WsClient {
    private:
        using OpenHandler = std::function<net::awaitable<void>(boost::system::error_code)>;
        using MessageHandler = std::function<net::awaitable<void>(WebSocketMessage)>;
        using CloseHandler = std::function<net::awaitable<void>()>;

        std::shared_ptr<WsClientImpl> m_impl; ///< Connection state.

    public:
        /**
         * @brief Create a closed client.
         * @param executor Executor the client's strand is made from.
         * @param host Server host.
         * @param port Server port.
         * @param scheme Transport.
         */
        WsClient(
            net::any_io_executor const& executor,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        );

        WsClient(
            net::io_context& context,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        );

        WsClient(WsClient const&) = delete;
        auto operator=(WsClient const&) -> WsClient& = delete;

        /**
         * @brief Abort the connection; pending operations complete with operation_aborted.
         */
        ~WsClient();

        /**
         * @brief Connect and perform the WebSocket handshake.
         * @param target Handshake target, e.g. "/ws".
         * @param headers Extra handshake fields.
         * @param timeout Limit for resolve, connect, and handshake.
         * @param ec Receives the error.
         * @note A no-op while connected; close first to switch targets. httplib probed the
         *       connection with a ping here.
         */
        auto AsyncConnect(
            std::string_view target,
            Headers const& headers,
            std::chrono::steady_clock::duration timeout,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        /**
         * @brief Send one message.
         * @param message Payload and frame type.
         * @param ec Receives the error; Error::NotInitialized when not connected.
         */
        auto AsyncSend(
            WebSocketMessage const& message,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        /**
         * @brief Send one message from any thread.
         * @param message Payload and frame type.
         * @return Future of the result.
         */
        auto Send(WebSocketMessage message) -> std::future<boost::system::error_code>;

        /**
         * @brief Read the next message; control frames are handled internally.
         * @param message Receives the payload and frame type.
         * @param ec Receives the error; websocket::error::closed after the peer's close frame.
         */
        auto AsyncRead(
            WebSocketMessage& message,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        /**
         * @brief Send a ping.
         * @param payload At most 125 bytes; longer fails with net::error::message_size (httplib
         *        threw std::length_error).
         * @param ec Receives the error.
         */
        auto AsyncPing(
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        auto Ping(std::string payload = {}) -> std::future<boost::system::error_code>;

        /**
         * @brief Send an unsolicited pong.
         * @param payload At most 125 bytes.
         * @param ec Receives the error.
         */
        auto AsyncPong(
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        auto Pong(std::string payload = {}) -> std::future<boost::system::error_code>;

        /**
         * @brief Close gracefully with a close frame, then drop the connection.
         * @param reason Close reason; cut to the 123 bytes a close frame allows.
         * @param ec Receives the close error.
         * @note Waiting for the peer's close frame is bounded by 5 s.
         */
        auto AsyncClose(
            std::string_view reason,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        auto AsyncClose(boost::system::error_code& ec) -> net::awaitable<void>;
        auto Close(std::string reason = "normal") -> std::future<boost::system::error_code>;

        /**
         * @brief Drop the connection without a close frame.
         */
        auto AsyncAbort() -> net::awaitable<void>;
        auto Abort() -> std::future<void>;

        /**
         * @brief Report whether the connection is open.
         * @return Snapshot.
         */
        [[nodiscard]] auto IsOpen() const -> bool;

        [[nodiscard]] auto Logger() const -> std::shared_ptr<spdlog::logger>;
        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void;

        /**
         * @brief Connect and run a session in the background.
         *
         * Calls `open(ec)` after connecting (also on failure), then `message(msg)` for every
         * message, and `close()` once the connection ended. Each handler may return void or
         * net::awaitable<void>. The connect timeout is 30 s.
         *
         * @param target Handshake target.
         * @param open Open handler.
         * @param message Message handler.
         * @param close Close handler.
         * @param headers Extra handshake fields.
         */
        template <typename OpenFunc, typename MessageFunc, typename CloseFunc>
        auto Run(
            std::string_view target,
            OpenFunc&& open,
            MessageFunc&& message,
            CloseFunc&& close,
            Headers const& headers = {}
        ) -> void {
            runImpl(
                target,
                OpenHandler{ detail::make_coro_handler<boost::system::error_code>(std::forward<OpenFunc>(open)) },
                MessageHandler{ detail::make_coro_handler<WebSocketMessage>(std::forward<MessageFunc>(message)) },
                CloseHandler{ detail::make_coro_handler<>(std::forward<CloseFunc>(close)) },
                headers
            );
        }

        /**
         * @brief Connect, then read in the background (see Run()).
         * @param target Handshake target.
         * @param headers Extra handshake fields.
         * @param message Message handler.
         * @param close Close handler.
         * @param ec Receives the connect error; no handler runs on failure.
         */
        template <typename MessageFunc, typename CloseFunc>
        auto AsyncRun(
            std::string_view target,
            Headers const& headers,
            MessageFunc&& message,
            CloseFunc&& close,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await asyncRunImpl(
                target,
                headers,
                MessageHandler{ detail::make_coro_handler<WebSocketMessage>(std::forward<MessageFunc>(message)) },
                CloseHandler{ detail::make_coro_handler<>(std::forward<CloseFunc>(close)) },
                ec
            );
        }

    private:
        auto runImpl(
            std::string_view target,
            OpenHandler open,
            MessageHandler message,
            CloseHandler close,
            Headers const& headers
        ) -> void;

        auto asyncRunImpl(
            std::string_view target,
            Headers const& headers,
            MessageHandler message,
            CloseHandler close,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;
    };

} // namespace mhttp::client

namespace mhttp::client {

    namespace websocket = beast::websocket;

    /**
     * @brief State behind WsClient; the stream is used on the strand only.
     */
    class WsClientImpl : public std::enable_shared_from_this<WsClientImpl> {
    public:
        using StreamType = websocket::stream<beast::basic_stream<tcp, net::any_io_executor, beast::simple_rate_policy>>;

        static constexpr std::size_t MAX_CONTROL_PAYLOAD{ 125 }; ///< RFC 6455 §5.5.
        static constexpr std::size_t MAX_CLOSE_REASON{ 123 };    ///< Control payload minus the code.

    private:
        net::strand<net::any_io_executor> m_strand;         ///< Serializes all I/O.
        tcp::resolver m_resolver;                           ///< Cancellable name resolution.
        std::string const m_host;                           ///< Server host.
        std::uint16_t const m_port;                         ///< Server port.
        url::Scheme const m_scheme;                         ///< Transport.
        detail::LoggerHolder m_logger{ "mhttp.ws_client" }; ///< Replaceable logger.
        std::atomic<std::shared_ptr<StreamType>> m_stream;  ///< Open connection; null when closed.
        std::atomic<bool> m_open{ false };                  ///< Thread-safe snapshot of handshake completion.
        detail::StrandMutex m_write_mutex;                  ///< Queues writes and control frames.
        bool m_reading{ false };                            ///< A read is in flight; strand only.
        bool m_connecting{ false };                         ///< A handshake is in flight; strand only.

    public:
        WsClientImpl(
            net::any_io_executor const& executor,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme
        ) : m_strand{ net::make_strand(executor) }, m_resolver{ m_strand }, m_host{ host }, m_port{ port }, m_scheme{ scheme }, m_write_mutex{ m_strand } {}

        [[nodiscard]] auto Logger() const -> detail::LoggerHolder const& {
            return m_logger;
        }

        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
            m_logger.SetLogger(std::move(logger));
        }

        [[nodiscard]] auto IsOpen() const -> bool {
            return m_open.load();
        }

        auto Connect(
            std::string_view target,
            Headers const& headers,
            std::chrono::steady_clock::duration timeout,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await onStrand([&]() -> net::awaitable<void> { co_await connectOnStrand(target, headers, timeout, ec); });
        }

        auto Send(
            WebSocketMessage const& message,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await onStrand([&]() -> net::awaitable<void> {
                auto const stream{ openStream(ec) };
                if (!stream) {
                    co_return;
                }
                if ((ec = co_await m_write_mutex.Lock())) {
                    co_return;
                }
                detail::StrandLock const lock{ m_write_mutex };
                if (stream != m_stream.load() || !m_open.load()) {
                    ec = net::error::operation_aborted;
                    co_return;
                }
                stream->binary(message.IsBinary());
                co_await stream->async_write(net::buffer(message.View()), net::redirect_error(net::use_awaitable, ec));
                if (ec) {
                    m_logger.GetLogger()->warn("ws send failed: {}", ec.message());
                    abortStream(stream);
                }
            });
        }

        auto Read(
            WebSocketMessage& message,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await onStrand([&]() -> net::awaitable<void> {
                auto const stream{ openStream(ec) };
                if (!stream) {
                    co_return;
                }
                if (m_reading) {
                    ec = Error::OperationInProgress;
                    co_return;
                }
                m_reading = true;
                message.Data().clear();
                auto buffer{ net::dynamic_buffer(message.Data()) };
                co_await stream->async_read(buffer, net::redirect_error(net::use_awaitable, ec));
                m_reading = false;
                if (ec) {
                    if (ec != websocket::error::closed) {
                        m_logger.GetLogger()->warn("ws read failed: {}", ec.message());
                    }
                    abortStream(stream);
                    co_return;
                }
                message.SetBinary(stream->got_binary());
            });
        }

        auto Control(
            bool ping,
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await onStrand([&]() -> net::awaitable<void> {
                if (payload.size() > MAX_CONTROL_PAYLOAD) {
                    ec = net::error::message_size;
                    co_return;
                }
                auto const stream{ openStream(ec) };
                if (!stream) {
                    co_return;
                }
                if ((ec = co_await m_write_mutex.Lock())) {
                    co_return;
                }
                detail::StrandLock const lock{ m_write_mutex };
                if (stream != m_stream.load() || !m_open.load()) {
                    ec = net::error::operation_aborted;
                    co_return;
                }
                websocket::ping_data data;
                data.assign(payload.data(), payload.size());
                if (ping) {
                    co_await stream->async_ping(data, net::redirect_error(net::use_awaitable, ec));
                } else {
                    co_await stream->async_pong(data, net::redirect_error(net::use_awaitable, ec));
                }
                if (ec) {
                    m_logger.GetLogger()->trace("ws {} failed: {}", ping ? "ping" : "pong", ec.message());
                    abortStream(stream);
                }
            });
        }

        auto CloseGracefully(
            std::string_view reason,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await onStrand([&]() -> net::awaitable<void> {
                auto const stream{ openStream(ec) };
                if (!stream) {
                    co_return;
                }
                if ((ec = co_await m_write_mutex.Lock())) {
                    co_return;
                }
                detail::StrandLock const lock{ m_write_mutex };
                if (stream != m_stream.load() || !m_open.load()) {
                    ec = net::error::operation_aborted;
                    co_return;
                }
                m_open.store(false);
                websocket::close_reason close_reason{ websocket::close_code::normal };
                close_reason.reason.assign(reason.data(), std::min(reason.size(), MAX_CLOSE_REASON));
                co_await stream->async_close(close_reason, net::redirect_error(net::use_awaitable, ec));
                if (ec && ec != net::error::operation_aborted) {
                    m_logger.GetLogger()->warn("ws close failed: {}", ec.message());
                }
                abortStream(stream);
            });
        }

        auto AsyncAbort() -> net::awaitable<void> {
            co_await onStrand([&]() -> net::awaitable<void> {
                abortNow();
                co_return;
            });
        }

        auto Abort() -> std::future<void> {
            return net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<void> {
                    self->abortNow();
                    co_return;
                },
                net::use_future
            );
        }

        /**
         * @brief Run `body` on the strand; the caller resumes on its own executor.
         * @tparam Body Coroutine factory.
         * @param body Work to run.
         */
        template <typename Body>
        auto onStrand(Body&& body) -> net::awaitable<void> {
            co_await net::co_spawn(m_strand, std::forward<Body>(body), net::use_awaitable);
        }

        [[nodiscard]] auto Strand() const -> net::strand<net::any_io_executor> {
            return m_strand;
        }

    private:
        /**
         * @brief Resolve, connect, and handshake; strand only.
         */
        auto connectOnStrand(
            std::string_view target,
            Headers const& headers,
            std::chrono::steady_clock::duration timeout,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            ec = {};
            if (m_connecting || (!IsOpen() && m_reading)) {
                ec = Error::OperationInProgress;
                co_return;
            }
            if (IsOpen()) {
                m_logger.GetLogger()->warn("ws connect ignored: already connected to {}:{}, target {} ignored (close first)", m_host, m_port, target);
                co_return;
            }
            if (m_scheme == url::Scheme::Tls) {
                ec = net::error::operation_not_supported;
                co_return;
            }
            auto const stream{ std::make_shared<StreamType>(m_strand) };
            m_stream.store(stream);
            m_connecting = true;
            auto const timed_out{ std::make_shared<bool>(false) };
            net::steady_timer deadline{ m_strand, timeout };
            deadline.async_wait([self = shared_from_this(), stream, timed_out](boost::system::error_code timer_ec) {
                if (!timer_ec && self->m_stream.load() == stream) {
                    *timed_out = true;
                    self->abortNow();
                }
            });

            /**
             * @brief Cancel the deadline and release the handshake slot on every exit.
             */
            struct ConnectGuard {
                WsClientImpl& owner;
                net::steady_timer& deadline;
                std::shared_ptr<StreamType> stream;

                ~ConnectGuard() {
                    deadline.cancel();
                    owner.m_connecting = false;
                    if (!owner.IsOpen()) {
                        owner.abortStream(stream);
                    }
                }
            } const guard{ *this, deadline, stream };

            auto& lowest{ beast::get_lowest_layer(*stream) };
            // One deadline includes DNS and the upgrade handshake.

            boost::system::error_code address_ec;
            auto const address{ net::ip::make_address(m_host, address_ec) };
            if (!address_ec) {
                co_await lowest.async_connect(tcp::endpoint{ address, m_port }, net::redirect_error(net::use_awaitable, ec));
            } else {
                auto const endpoints{ co_await m_resolver.async_resolve(m_host, std::to_string(m_port), net::redirect_error(net::use_awaitable, ec)) };
                if (!ec && m_stream.load() == stream) {
                    co_await lowest.async_connect(endpoints, net::redirect_error(net::use_awaitable, ec));
                }
            }
            if (*timed_out) {
                ec = beast::error::timeout;
            } else if (m_stream.load() != stream) {
                ec = net::error::operation_aborted;
            }
            if (ec) {
                m_logger.GetLogger()->error("ws connect {}:{} failed: {}", m_host, m_port, ec.message());
                co_return;
            }
            boost::system::error_code option_ec;
            lowest.socket().set_option(tcp::no_delay{ true }, option_ec);

            // The decorator outlives this call inside the stream: give it owned copies.
            Headers extra;
            extra.Merge(headers);
            stream->set_option(websocket::stream_base::decorator([origin = url::make_url_value(m_host, m_port, m_scheme), extra](websocket::request_type& request) {
                request.set(http::field::origin, origin);
                request.set(http::field::user_agent, std::string{ BOOST_BEAST_VERSION_STRING } + " mhttp-ws-client");
                for (auto const& header : extra) {
                    request.set(header.name_string, header.value);
                }
            }));
            co_await stream->async_handshake(url::make_host_value(m_host, m_port, m_scheme), target, net::redirect_error(net::use_awaitable, ec));
            if (*timed_out) {
                ec = beast::error::timeout;
            } else if (m_stream.load() != stream) {
                ec = net::error::operation_aborted;
            }
            if (ec) {
                m_logger.GetLogger()->error("ws handshake with {}:{}{} failed: {}", m_host, m_port, target, ec.message());
                co_return;
            }
            // Hand timeouts over to the WebSocket layer: none while idle, 5 s for the close
            // handshake (httplib raced async_close against a timer instead).
            lowest.expires_never();
            websocket::stream_base::timeout options{};
            options.handshake_timeout = std::chrono::seconds{ 5 };
            options.idle_timeout = websocket::stream_base::none();
            options.keep_alive_pings = false;
            stream->set_option(options);
            m_open.store(true);
            m_logger.GetLogger()->debug("ws connected to {}:{}{}", m_host, m_port, target);
        }

        auto openStream(boost::system::error_code& ec) const -> std::shared_ptr<StreamType> {
            ec = {};
            if (auto const stream{ m_stream.load() }; stream && m_open.load()) {
                return stream;
            }
            ec = Error::NotInitialized;
            return nullptr;
        }

        /**
         * @brief Close the socket; pending operations complete with operation_aborted.
         */
        auto abortNow() -> void {
            m_open.store(false);
            m_resolver.cancel();
            if (auto const stream{ m_stream.exchange(nullptr) }) {
                beast::get_lowest_layer(*stream).close();
            }
        }

        /**
         * @brief An old operation may only abort its own connection.
         */
        auto abortStream(std::shared_ptr<StreamType> const& stream) -> void {
            if (m_stream.load() == stream) {
                abortNow();
            }
        }
    };

    WsClient::WsClient(
        net::any_io_executor const& executor,
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) : m_impl{ std::make_shared<WsClientImpl>(executor, host, port, scheme) } {}

    WsClient::WsClient(
        net::io_context& context,
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) : WsClient{ context.get_executor(), host, port, scheme } {}

    WsClient::~WsClient() {
        static_cast<void>(m_impl->Abort());
    }

    auto WsClient::AsyncConnect(
        std::string_view target,
        Headers const& headers,
        std::chrono::steady_clock::duration timeout,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->Connect(target, headers, timeout, ec);
    }

    auto WsClient::AsyncSend(
        WebSocketMessage const& message,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->Send(message, ec);
    }

    auto WsClient::Send(WebSocketMessage message) -> std::future<boost::system::error_code> {
        auto const impl{ m_impl };
        return net::co_spawn(
            impl->Strand(),
            [impl, message = std::move(message)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await impl->Send(message, ec);
                co_return ec;
            },
            net::use_future
        );
    }

    auto WsClient::AsyncRead(
        WebSocketMessage& message,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->Read(message, ec);
    }

    auto WsClient::AsyncPing(
        std::string_view payload,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->Control(true, payload, ec);
    }

    auto WsClient::Ping(std::string payload) -> std::future<boost::system::error_code> {
        auto const impl{ m_impl };
        return net::co_spawn(
            impl->Strand(),
            [impl, payload = std::move(payload)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await impl->Control(true, payload, ec);
                co_return ec;
            },
            net::use_future
        );
    }

    auto WsClient::AsyncPong(
        std::string_view payload,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->Control(false, payload, ec);
    }

    auto WsClient::Pong(std::string payload) -> std::future<boost::system::error_code> {
        auto const impl{ m_impl };
        return net::co_spawn(
            impl->Strand(),
            [impl, payload = std::move(payload)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await impl->Control(false, payload, ec);
                co_return ec;
            },
            net::use_future
        );
    }

    auto WsClient::AsyncClose(
        std::string_view reason,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->CloseGracefully(reason, ec);
    }

    auto WsClient::AsyncClose(boost::system::error_code& ec) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->CloseGracefully("normal", ec);
    }

    auto WsClient::Close(std::string reason) -> std::future<boost::system::error_code> {
        auto const impl{ m_impl };
        return net::co_spawn(
            impl->Strand(),
            [impl, reason = std::move(reason)]() -> net::awaitable<boost::system::error_code> {
                boost::system::error_code ec;
                co_await impl->CloseGracefully(reason, ec);
                co_return ec;
            },
            net::use_future
        );
    }

    auto WsClient::AsyncAbort() -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->AsyncAbort();
    }

    auto WsClient::Abort() -> std::future<void> {
        return m_impl->Abort();
    }

    auto WsClient::IsOpen() const -> bool {
        return m_impl->IsOpen();
    }

    auto WsClient::Logger() const -> std::shared_ptr<spdlog::logger> {
        return m_impl->Logger().GetLogger();
    }

    auto WsClient::SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
        m_impl->SetLogger(std::move(logger));
    }

    namespace {

        /**
         * @brief Read messages until the connection ends, then run the close handler.
         * @param impl Client state.
         * @param message Message handler.
         * @param close Close handler.
         */
        auto read_loop(
            std::shared_ptr<WsClientImpl> impl,
            std::function<net::awaitable<void>(WebSocketMessage)> message,
            std::function<net::awaitable<void>()> close
        ) -> net::awaitable<void> {
            for (;;) {
                WebSocketMessage received;
                boost::system::error_code ec;
                co_await impl->Read(received, ec);
                if (ec) {
                    break;
                }
                try {
                    co_await message(std::move(received));
                } catch (std::exception const& e) {
                    impl->Logger().GetLogger()->error("ws message handler failed: {}", e.what());
                }
            }
            try {
                co_await close();
            } catch (std::exception const& e) {
                impl->Logger().GetLogger()->error("ws close handler failed: {}", e.what());
            }
            impl->Logger().GetLogger()->debug("ws session ended");
        }

    } // namespace

    auto WsClient::runImpl(
        std::string_view target,
        OpenHandler open,
        MessageHandler message,
        CloseHandler close,
        Headers const& headers
    ) -> void {
        Headers owned;
        owned.Merge(headers);
        net::co_spawn(
            m_impl->Strand(),
            [impl = m_impl, target = std::string{ target }, owned = std::move(owned), open = std::move(open), message = std::move(message), close = std::move(close)]() mutable -> net::awaitable<void> {
                boost::system::error_code ec;
                co_await impl->Connect(target, owned, std::chrono::seconds{ 30 }, ec);
                try {
                    co_await open(ec);
                } catch (std::exception const& e) {
                    impl->Logger().GetLogger()->error("ws open handler failed: {}", e.what());
                }
                if (!ec) {
                    co_await read_loop(impl, std::move(message), std::move(close));
                }
            },
            net::detached
        );
    }

    auto WsClient::asyncRunImpl(
        std::string_view target,
        Headers const& headers,
        MessageHandler message,
        CloseHandler close,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        auto const impl{ m_impl };
        co_await impl->Connect(target, headers, std::chrono::seconds{ 30 }, ec);
        if (ec) {
            co_return;
        }
        net::co_spawn(impl->Strand(), read_loop(impl, std::move(message), std::move(close)), net::detached);
    }

} // namespace mhttp::client
