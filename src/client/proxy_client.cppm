/**
 * @file proxy_client.cppm
 * @brief Provide ProxyClient: open a CONNECT tunnel through an HTTP proxy and relay raw bytes.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:proxy_client;

import std;
import :config;
import :error;
import :logging;
import :url;
import :headers;
import :http_stream;

namespace mhttp::client {

    class ProxyClientImpl;

} // namespace mhttp::client

export namespace mhttp::client {

    /**
     * @brief Byte tunnel through an HTTP proxy (port of httplib::client::proxy_client).
     *
     * AsyncConnect() connects to the proxy, sends "CONNECT target", and on a 2xx answer turns the
     * connection into a raw tunnel to the target. Operations run on the client's own strand
     * and may be started from any executor; do not overlap two reads or two writes. TLS to the
     * proxy is not ported: an HTTPS proxy fails with net::error::operation_not_supported.
     */
    class ProxyClient {
    private:
        std::shared_ptr<ProxyClientImpl> m_impl; ///< Tunnel state.

    public:
        /**
         * @brief Create a closed tunnel client for a proxy.
         * @param executor Executor the client's strand is made from.
         * @param host Proxy host.
         * @param port Proxy port.
         * @param scheme Transport to the proxy.
         */
        ProxyClient(
            net::any_io_executor const& executor,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        );

        ProxyClient(
            net::io_context& context,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        );

        ProxyClient(ProxyClient const&) = delete;
        auto operator=(ProxyClient const&) -> ProxyClient& = delete;

        /**
         * @brief Close the tunnel.
         */
        ~ProxyClient();

        /**
         * @brief Open the tunnel; a no-op while it is open (close first to switch targets).
         * @param target Authority to tunnel to, e.g. "example.com:443".
         * @param headers Extra CONNECT fields such as Proxy-Authorization.
         * @param ec Receives the connect error, or Error::HttpStatus when the proxy refused
         *        (see HttpStatus()).
         */
        auto AsyncConnect(
            std::string_view target,
            Headers const& headers,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        /**
         * @brief Read tunnel bytes; bytes the proxy sent right after its answer come first.
         * @param buffer Destination.
         * @param ec Receives the read error (the tunnel then closes) or Error::NotInitialized.
         * @return Bytes read.
         */
        auto AsyncReadSome(
            net::mutable_buffer buffer,
            boost::system::error_code& ec
        ) -> net::awaitable<std::size_t>;

        /**
         * @brief Write all bytes into the tunnel.
         * @param buffer Bytes to send.
         * @param ec Receives the write error (the tunnel then closes) or Error::NotInitialized.
         */
        auto AsyncWrite(
            net::const_buffer buffer,
            boost::system::error_code& ec
        ) -> net::awaitable<void>;

        /**
         * @brief Close the tunnel from any thread.
         * @return Future ready once closed; do not wait on it from the client's I/O thread.
         */
        auto Close() -> std::future<void>;
        auto AsyncClose() -> net::awaitable<void>;

        [[nodiscard]] auto IsOpen() const -> bool;

        /**
         * @brief Return the status of the last CONNECT answer.
         * @return 2xx after success, the refusal status (e.g. 407), or Status::unknown when no
         *         answer arrived.
         */
        [[nodiscard]] auto HttpStatus() const -> Status;

        /**
         * @brief Bound connecting and the CONNECT exchange; tunnel traffic has no timeout.
         * @param timeout Limit; default 30 s.
         */
        auto SetTimeout(std::chrono::steady_clock::duration timeout) -> void;

        [[nodiscard]] auto Logger() const -> std::shared_ptr<spdlog::logger>;
        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void;
    };

} // namespace mhttp::client

namespace mhttp::client {

    /**
     * @brief State behind ProxyClient; stream and buffer are used on the strand only.
     */
    class ProxyClientImpl : public std::enable_shared_from_this<ProxyClientImpl> {
    private:
        net::strand<net::any_io_executor> m_strand;                                               ///< Serializes tunnel I/O.
        tcp::resolver m_resolver;                                                                 ///< Proxy name resolution.
        std::string const m_host;                                                                 ///< Proxy host.
        std::uint16_t const m_port;                                                               ///< Proxy port.
        url::Scheme const m_scheme;                                                               ///< Transport to the proxy.
        detail::LoggerHolder m_logger{ "mhttp.proxy_client" };                                    ///< Replaceable logger.
        std::atomic<std::chrono::steady_clock::duration> m_timeout{ std::chrono::seconds{ 30 } }; ///< Handshake limit.
        std::atomic<std::shared_ptr<HttpStream>> m_stream;                                        ///< Open tunnel; null when closed.
        std::atomic<bool> m_open{ false };                                                        ///< Completed CONNECT snapshot.
        std::atomic<Status> m_status{ Status::unknown };                                          ///< Last CONNECT answer.
        beast::flat_buffer m_buffer;                                                              ///< Tunnel bytes read with the answer.
        bool m_connecting{ false };                                                               ///< Handshake in progress; strand only.
        bool m_reading{ false };                                                                  ///< Read in progress; strand only.
        bool m_writing{ false };                                                                  ///< Write in progress; strand only.

        /**
         * @brief Clear an operation's busy flag on every exit.
         */
        struct BusyGuard {
            bool& busy;

            explicit BusyGuard(bool& flag) : busy{ flag } { busy = true; }

            ~BusyGuard() { busy = false; }
        };

    public:
        ProxyClientImpl(
            net::any_io_executor const& executor,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme
        ) : m_strand{ net::make_strand(executor) }, m_resolver{ m_strand }, m_host{ host }, m_port{ port }, m_scheme{ scheme } {}

        auto Connect(
            std::string_view target,
            Headers const& headers,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await net::co_spawn(
                m_strand,
                [&, self = shared_from_this()]() -> net::awaitable<void> {
                    co_await connectOnStrand(target, headers, ec);
                },
                net::use_awaitable
            );
        }

        auto ReadSome(
            net::mutable_buffer buffer,
            boost::system::error_code& ec
        ) -> net::awaitable<std::size_t> {
            co_return co_await net::co_spawn(
                m_strand,
                [&, self = shared_from_this()]() -> net::awaitable<std::size_t> {
                    ec = {};
                    auto const stream{ openStream(ec) };
                    if (!stream) {
                        co_return 0;
                    }
                    if (m_reading) {
                        ec = Error::OperationInProgress;
                        co_return 0;
                    }
                    BusyGuard const guard{ m_reading };
                    // httplib read the answer into a local buffer and dropped whatever followed it.
                    if (m_buffer.size() > 0) {
                        auto const copied{ net::buffer_copy(buffer, m_buffer.data()) };
                        m_buffer.consume(copied);
                        co_return copied;
                    }
                    auto const bytes{ co_await stream->async_read_some(buffer, net::redirect_error(net::use_awaitable, ec)) };
                    if (ec) {
                        closeNow();
                    }
                    co_return bytes;
                },
                net::use_awaitable
            );
        }

        auto Write(
            net::const_buffer buffer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await net::co_spawn(
                m_strand,
                [&, self = shared_from_this()]() -> net::awaitable<void> {
                    auto const stream{ openStream(ec) };
                    if (!stream) {
                        co_return;
                    }
                    if (m_writing) {
                        ec = Error::OperationInProgress;
                        co_return;
                    }
                    BusyGuard const guard{ m_writing };
                    co_await net::async_write(*stream, buffer, net::redirect_error(net::use_awaitable, ec));
                    if (ec) {
                        closeNow();
                    }
                },
                net::use_awaitable
            );
        }

        auto AsyncClose() -> net::awaitable<void> {
            co_await net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<void> {
                    self->closeNow();
                    co_return;
                },
                net::use_awaitable
            );
        }

        auto Close() -> std::future<void> {
            return net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<void> {
                    self->closeNow();
                    co_return;
                },
                net::use_future
            );
        }

        [[nodiscard]] auto IsOpen() const -> bool {
            return m_open.load();
        }

        [[nodiscard]] auto LastStatus() const -> Status {
            return m_status.load();
        }

        auto SetTimeout(std::chrono::steady_clock::duration timeout) -> void {
            m_timeout.store(timeout);
        }

        [[nodiscard]] auto Logger() const -> detail::LoggerHolder const& {
            return m_logger;
        }

        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
            m_logger.SetLogger(std::move(logger));
        }

    private:
        /**
         * @brief Connect to the proxy and exchange CONNECT; strand only.
         * @param target Tunnel authority.
         * @param headers Extra CONNECT fields.
         * @param ec Receives the error.
         */
        auto connectOnStrand(
            std::string_view target,
            Headers const& headers,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            ec = {};
            if (m_connecting || (!IsOpen() && (m_reading || m_writing))) {
                ec = Error::OperationInProgress;
                co_return;
            }
            if (IsOpen()) {
                m_logger.GetLogger()->warn("proxy connect ignored: already connected to {}:{}, target {} ignored (close first)", m_host, m_port, target);
                co_return;
            }
            m_status.store(Status::unknown);
            m_buffer.clear();
            if (m_scheme == url::Scheme::Tls) {
                ec = net::error::operation_not_supported;
                co_return;
            }
            auto const stream{ std::make_shared<HttpStream>(HttpStream::PlainStream{ m_strand }) };
            m_stream.store(stream);
            BusyGuard const busy{ m_connecting };
            auto const timed_out{ std::make_shared<bool>(false) };
            net::steady_timer deadline{ m_strand, m_timeout.load() };
            deadline.async_wait([self = shared_from_this(), stream, timed_out](boost::system::error_code timer_ec) {
                if (!timer_ec && self->m_stream.load() == stream) {
                    *timed_out = true;
                    self->closeNow();
                }
            });

            /**
             * @brief Cancel the deadline and close an unsuccessful handshake.
             */
            struct ConnectGuard {
                ProxyClientImpl& owner;
                net::steady_timer& deadline;

                ~ConnectGuard() {
                    deadline.cancel();
                    if (!owner.IsOpen()) {
                        owner.closeNow();
                    }
                }
            } const guard{ *this, deadline };

            auto const check_cancelled{ [&] {
                if (*timed_out) {
                    ec = beast::error::timeout;
                } else if (m_stream.load() != stream) {
                    ec = net::error::operation_aborted;
                }
            } };
            boost::system::error_code address_ec;
            auto const address{ net::ip::make_address(m_host, address_ec) };
            if (!address_ec) {
                co_await stream->AsyncConnect(tcp::endpoint{ address, m_port }, ec);
            } else {
                auto const endpoints{ co_await m_resolver.async_resolve(m_host, std::to_string(m_port), net::redirect_error(net::use_awaitable, ec)) };
                if (!ec && m_stream.load() == stream) {
                    co_await stream->AsyncConnect(endpoints, ec);
                }
            }
            check_cancelled();
            if (ec) {
                m_logger.GetLogger()->error("proxy connect {}:{} failed: {}", m_host, m_port, ec.message());
                co_return;
            }

            http::request<http::empty_body> request{ http::verb::connect, target, 11 };
            request.set(http::field::host, target);
            for (auto const& header : headers) {
                request.set(header.name_string, header.value);
            }
            co_await http::async_write(*stream, request, net::redirect_error(net::use_awaitable, ec));
            check_cancelled();
            if (ec) {
                m_logger.GetLogger()->error("proxy write CONNECT to {}:{} failed: {}", m_host, m_port, ec.message());
                co_return;
            }
            unsigned code{ 0 };
            do {
                http::response_parser<http::empty_body> parser;
                co_await http::async_read_header(*stream, m_buffer, parser, net::redirect_error(net::use_awaitable, ec));
                check_cancelled();
                if (ec) {
                    m_logger.GetLogger()->error("proxy read CONNECT answer from {}:{} failed: {}", m_host, m_port, ec.message());
                    co_return;
                }
                code = parser.get().result_int();
                m_status.store(parser.get().result());
            } while (code >= 100 && code < 200 && code != 101);
            if (code < 200 || code >= 300) {
                m_buffer.clear();
                ec = Error::HttpStatus;
                m_logger.GetLogger()->error("proxy {}:{} refused CONNECT {}: status {}", m_host, m_port, target, code);
                co_return;
            }
            stream->ExpiresNever();
            m_open.store(true);
            m_logger.GetLogger()->trace("proxy {}:{} tunnel to {} open", m_host, m_port, target);
        }

        /**
         * @brief Return the open stream.
         * @param ec Receives Error::NotInitialized when no tunnel is open.
         * @return Stream, or null.
         */
        auto openStream(boost::system::error_code& ec) const -> std::shared_ptr<HttpStream> {
            ec = {};
            if (auto const stream{ m_stream.load() }; stream && m_open.load()) {
                return stream;
            }
            ec = Error::NotInitialized;
            return nullptr;
        }

        auto closeNow() -> void {
            m_open.store(false);
            m_resolver.cancel();
            if (auto const stream{ m_stream.exchange(nullptr) }) {
                boost::system::error_code ec;
                stream->Close(ec);
            }
            m_buffer.clear();
        }
    };

    ProxyClient::ProxyClient(
        net::any_io_executor const& executor,
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) : m_impl{ std::make_shared<ProxyClientImpl>(executor, host, port, scheme) } {}

    ProxyClient::ProxyClient(
        net::io_context& context,
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) : ProxyClient{ context.get_executor(), host, port, scheme } {}

    ProxyClient::~ProxyClient() {
        static_cast<void>(m_impl->Close());
    }

    auto ProxyClient::AsyncConnect(
        std::string_view target,
        Headers const& headers,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        co_await m_impl->Connect(target, headers, ec);
    }

    auto ProxyClient::AsyncReadSome(
        net::mutable_buffer buffer,
        boost::system::error_code& ec
    ) -> net::awaitable<std::size_t> {
        co_return co_await m_impl->ReadSome(buffer, ec);
    }

    auto ProxyClient::AsyncWrite(
        net::const_buffer buffer,
        boost::system::error_code& ec
    ) -> net::awaitable<void> {
        co_await m_impl->Write(buffer, ec);
    }

    auto ProxyClient::Close() -> std::future<void> {
        return m_impl->Close();
    }

    auto ProxyClient::AsyncClose() -> net::awaitable<void> {
        co_await m_impl->AsyncClose();
    }

    auto ProxyClient::IsOpen() const -> bool {
        return m_impl->IsOpen();
    }

    auto ProxyClient::HttpStatus() const -> Status {
        return m_impl->LastStatus();
    }

    auto ProxyClient::SetTimeout(std::chrono::steady_clock::duration timeout) -> void {
        m_impl->SetTimeout(timeout);
    }

    auto ProxyClient::Logger() const -> std::shared_ptr<spdlog::logger> {
        return m_impl->Logger().GetLogger();
    }

    auto ProxyClient::SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
        m_impl->SetLogger(std::move(logger));
    }

} // namespace mhttp::client
