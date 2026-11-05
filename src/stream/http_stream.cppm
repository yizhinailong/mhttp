/**
 * @file http_stream.cppm
 * @brief Wrap plain TCP and TLS connections behind one asynchronous byte stream.
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/basic_stream.hpp>
#include <boost/beast/core/rate_policy.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/teardown.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:http_stream;

import std;
import :config;
import :tls;

namespace mhttp {

    /**
     * @brief Connection byte stream with per-operation timeouts.
     *
     * Satisfies Beast's AsyncReadStream and AsyncWriteStream, so http::async_read and
     * http::async_write accept it directly. The shared TLS context outlives the SSL stream.
     */
    class HttpStream {
    public:
        using PlainStream = beast::basic_stream<tcp, net::any_io_executor, beast::simple_rate_policy>;
        using TlsStream = net::ssl::stream<PlainStream>;
        using executor_type = PlainStream::executor_type;

    private:
        std::shared_ptr<void> m_context;               ///< Keeps TLS configuration alive.
        std::variant<PlainStream, TlsStream> m_stream; ///< Plain or encrypted stream.

    public:
        /**
         * @brief Take ownership of an accepted or created plain stream.
         * @param stream Stream to own.
         */
        explicit HttpStream(PlainStream&& stream) : m_stream{ std::move(stream) } {}

        /**
         * @brief Wrap an accepted connection, optionally encrypting it with a server identity.
         */
        HttpStream(
            PlainStream&& stream,
            std::shared_ptr<TlsServerContext> const& context
        ) : m_context{ context }, m_stream{ makeServerStream(std::move(stream), context) } {}

        /**
         * @brief Return whether this connection uses TLS.
         */
        [[nodiscard]] auto IsTls() const noexcept -> bool {
            return std::holds_alternative<TlsStream>(m_stream);
        }

        /**
         * @brief Expose the TCP layer for Beast's socket and deadline customization points.
         * @note Reads and writes must use HttpStream so encrypted connections retain TLS.
         */
        auto next_layer() -> PlainStream& { return plain(); }

        /**
         * @brief Borrow the TCP layer for const state queries.
         */
        auto next_layer() const -> PlainStream const& { return plain(); }

        /**
         * @brief Forward synchronous WebSocket teardown through the active protocol layer.
         */
        friend auto teardown(
            beast::role_type role,
            HttpStream& stream,
            boost::system::error_code& ec
        ) -> void {
            std::visit([&](auto& transport) {
                using beast::websocket::teardown;
                teardown(role, transport, ec);
            },
                       stream.m_stream);
        }

        /**
         * @brief Forward asynchronous WebSocket teardown, including TLS close_notify.
         */
        template <typename Handler>
        friend auto async_teardown(
            beast::role_type role,
            HttpStream& stream,
            Handler&& handler
        ) -> void {
            std::visit([&](auto& transport) {
                using beast::websocket::async_teardown;
                async_teardown(role, transport, std::forward<Handler>(handler));
            },
                       stream.m_stream);
        }

        /**
         * @brief Create a TLS client stream and configure SNI and host verification.
         * @param executor Connection executor.
         * @param host Expected server name or IP address.
         * @param context Shared trust configuration; null uses system roots.
         * @return Configured stream or a TLS configuration error.
         */
        static auto MakeTlsClient(
            net::any_io_executor executor,
            std::string const& host,
            std::shared_ptr<TlsClientContext> context = {}
        ) -> Result<std::shared_ptr<HttpStream>> {
            if (!context) {
                auto prepared{ detail::default_tls_client_context() };
                if (!prepared) {
                    return std::unexpected{ prepared.error() };
                }
                context = *prepared;
            }
            try {
                auto stream{ std::shared_ptr<HttpStream>{ new HttpStream{ executor, context } } };
                auto& tls{ std::get<TlsStream>(stream->m_stream) };
                boost::system::error_code ec;
                net::ip::make_address(host, ec);
                if (ec && ::SSL_set_tlsext_host_name(tls.native_handle(), host.c_str()) != 1) {
                    return std::unexpected{ detail::tls_error() };
                }
                if (context->m_verify_peer) {
                    tls.set_verify_callback(net::ssl::host_name_verification{ host }, ec);
                    if (ec) {
                        return std::unexpected{ ec };
                    }
                }
                return stream;
            } catch (boost::system::system_error const& error) {
                return std::unexpected{ error.code() };
            }
        }

        /**
         * @brief Complete a TLS client handshake under the stream's current deadline.
         */
        auto AsyncClientHandshake(boost::system::error_code& ec) -> net::awaitable<void> {
            if (auto* tls{ std::get_if<TlsStream>(&m_stream) }) {
                co_await tls->async_handshake(net::ssl::stream_base::client, net::redirect_error(net::use_awaitable, ec));
            }
        }

        /**
         * @brief Complete an accepted TLS handshake under the current connection deadline.
         */
        auto AsyncServerHandshake(boost::system::error_code& ec) -> net::awaitable<void> {
            if (auto* tls{ std::get_if<TlsStream>(&m_stream) }) {
                co_await tls->async_handshake(net::ssl::stream_base::server, net::redirect_error(net::use_awaitable, ec));
            }
        }

        /**
         * @brief Return the executor the stream's operations complete on.
         * @return The connection's executor (its strand on the server side).
         */
        auto get_executor() -> executor_type {
            return plain().get_executor();
        }

        /**
         * @brief Start an asynchronous read; part of the AsyncReadStream protocol.
         * @param buffers Destination buffers.
         * @param token Completion token.
         * @return Whatever the completion token produces.
         */
        template <typename MutableBufferSequence, typename ReadToken>
        auto async_read_some(MutableBufferSequence const& buffers, ReadToken&& token) {
            return std::visit([&](auto& stream) { return stream.async_read_some(buffers, std::forward<ReadToken>(token)); }, m_stream);
        }

        /**
         * @brief Start an asynchronous write; part of the AsyncWriteStream protocol.
         * @param buffers Source buffers.
         * @param token Completion token.
         * @return Whatever the completion token produces.
         */
        template <typename ConstBufferSequence, typename WriteToken>
        auto async_write_some(ConstBufferSequence const& buffers, WriteToken&& token) {
            return std::visit([&](auto& stream) { return stream.async_write_some(buffers, std::forward<WriteToken>(token)); }, m_stream);
        }

        /**
         * @brief Borrow the underlying socket.
         * @return Socket owned by this stream.
         */
        auto Socket() -> tcp::socket& {
            return plain().socket();
        }

        /**
         * @brief Report whether the socket is open.
         * @return Snapshot of the socket's open state.
         */
        [[nodiscard]] auto IsOpen() const -> bool {
            return plain().socket().is_open();
        }

        /**
         * @brief Bound the next asynchronous operations by a relative timeout.
         * @param expiry Time allowed before pending operations fail with beast::error::timeout.
         */
        auto ExpiresAfter(net::steady_timer::duration expiry) -> void {
            plain().expires_after(expiry);
        }

        /**
         * @brief Remove any pending timeout.
         */
        auto ExpiresNever() -> void {
            plain().expires_never();
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
            co_await plain().async_connect(endpoint, net::redirect_error(net::use_awaitable, ec));
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
            co_await plain().async_connect(endpoints, net::redirect_error(net::use_awaitable, ec));
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
            if (auto* tls{ std::get_if<TlsStream>(&m_stream) }; tls && ::SSL_pending(tls->native_handle()) != 0) {
                return false;
            }
            auto& socket{ Socket() };
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
            plain().rate_policy().read_limit(to_limit(read_bytes_per_second));
            plain().rate_policy().write_limit(to_limit(write_bytes_per_second));
        }

        /**
         * @brief Release the socket immediately.
         * @param ec Receives the last error; failures are not actionable at this point.
         * @note Shuts down only the send direction before closing. Shutting down the receive
         *       direction makes Windows answer later peer data (such as an unread request body)
         *       with RST, which discards a response the peer has not read yet (WSAECONNRESET).
         */
        auto Close(boost::system::error_code& ec) -> void {
            Socket().shutdown(net::socket_base::shutdown_send, ec);
            Socket().close(ec);
        }

        /**
         * @brief Finish the connection gracefully so the peer sees a clean EOF.
         * @note TLS shutdown is bounded by five seconds; aborts use Close() directly.
         */
        auto AsyncClose() -> net::awaitable<void> {
            boost::system::error_code ec;
            if (auto* tls{ std::get_if<TlsStream>(&m_stream) }; tls && IsOpen()) {
                ExpiresAfter(std::chrono::seconds{ 5 });
                co_await tls->async_shutdown(net::redirect_error(net::use_awaitable, ec));
            }
            Close(ec);
            co_return;
        }

    private:
        /**
         * @brief Build the selected server transport while retaining its certificate context.
         */
        static auto makeServerStream(
            PlainStream&& stream,
            std::shared_ptr<TlsServerContext> const& context
        ) -> std::variant<PlainStream, TlsStream> {
            if (context) {
                return std::variant<PlainStream, TlsStream>{ std::in_place_type<TlsStream>, std::move(stream), context->m_context };
            }
            return std::variant<PlainStream, TlsStream>{ std::in_place_type<PlainStream>, std::move(stream) };
        }

        /**
         * @brief Build an encrypted stream whose context has already been validated.
         */
        HttpStream(
            net::any_io_executor executor,
            std::shared_ptr<TlsClientContext> const& context
        ) : m_context{ context }, m_stream{ std::in_place_type<TlsStream>, executor, context->m_context } {}

        /**
         * @brief Borrow the common TCP layer of either stream alternative.
         */
        auto plain() -> PlainStream& {
            return std::visit([](auto& stream) -> PlainStream& {
                if constexpr (std::same_as<std::remove_cvref_t<decltype(stream)>, PlainStream>) {
                    return stream;
                } else {
                    return stream.next_layer();
                }
            },
                              m_stream);
        }

        /**
         * @brief Borrow the TCP layer for state queries.
         */
        auto plain() const -> PlainStream const& {
            return const_cast<HttpStream*>(this)->plain();
        }

        /**
         * @brief Apply client socket options after a successful connect.
         * @param ec Connect result; options are skipped on failure.
         */
        auto configureConnected(boost::system::error_code const& ec) -> void {
            if (ec) {
                return;
            }
            boost::system::error_code option_ec;
            Socket().set_option(tcp::no_delay{ true }, option_ec);
            Socket().set_option(net::socket_base::keep_alive{ true }, option_ec);
        }
    };

} // namespace mhttp
