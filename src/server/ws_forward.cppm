/**
 * @file ws_forward.cppm
 * @brief Gate downstream upgrades on an upstream handshake and relay bounded messages.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:ws_forward;

import std;
import :config;
import :strings;
import :url;
import :headers;
import :request;
import :connection;
import :async_event;
import :websocket_message;
import :websocket_conn;
import :ws_client;
import :proxy_common;

export namespace mhttp {

    /**
     * @brief Immutable settings of a single-upstream WebSocket forwarding route.
     */
    struct WsForwardConfig {
        bool strip_prefix{ true };                                                         ///< Replace the route prefix with the upstream base path.
        bool preserve_host{ false };                                                       ///< Retain the public Host in the upstream handshake.
        std::chrono::steady_clock::duration connect_timeout{ std::chrono::seconds{ 30 } }; ///< DNS, TCP, and handshake deadline.
        std::uint64_t message_limit{ 16 * 1024 * 1024 };                                   ///< Maximum complete message bytes in either direction; must be positive.
    };

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Own an upstream connection and its two relays on the downstream session strand.
     */
    class WsForwardState : public std::enable_shared_from_this<WsForwardState> {
    private:
        net::any_io_executor m_executor;         ///< Downstream strand and relay completion executor.
        std::shared_ptr<Connection> m_http;      ///< HTTP transport, released after the peer observer drains.
        client::WsClient m_upstream;             ///< Dedicated upstream connection, with serialized writes.
        WebSocketConn::WeakPtr m_downstream;     ///< Avoid a cycle with downstream callbacks.
        net::cancellation_signal m_watch_cancel; ///< Interrupt the HTTP peer observer.
        util::AsyncEvent m_watch_done;           ///< HTTP observer completion.
        util::AsyncEvent m_reader_done;          ///< Upstream read relay completion.
        bool m_cancelled{ false };               ///< Forced cancellation latch.
        bool m_watch_stopping{ false };          ///< Prevent a late observer from starting.
        bool m_reader_started{ false };          ///< The read relay must be drained.
        bool m_reader_finished{ false };         ///< Latched read relay completion.
        bool m_stopping{ false };                ///< A downstream close already owns the close handshake.

    public:
        /**
         * @brief Bind a fresh upstream and completion events to one HTTP session.
         */
        WsForwardState(
            std::shared_ptr<Connection> connection,
            url::UrlInfo const& upstream,
            std::uint64_t message_limit
        ) : m_executor{ connection->Executor() }, m_http{ std::move(connection) }, m_upstream{ m_executor, upstream.host, upstream.EffectivePort() }, m_watch_done{ m_executor }, m_reader_done{ m_executor } {
            m_upstream.SetMessageLimit(message_limit);
        }

        /**
         * @brief Cancel upstream I/O without waiting or blocking the downstream strand.
         */
        auto Cancel() -> void {
            m_cancelled = true;
            m_stopping = true;
            static_cast<void>(m_upstream.Abort());
        }

        /**
         * @brief Observe EOF while the upstream handshake is pending, without consuming frames.
         */
        auto Watch() -> void {
            m_http->SetAbortHandler([weak = weak_from_this()] { if (auto self{ weak.lock() }) { self->Cancel(); } });
            net::co_spawn(m_executor, [self = shared_from_this()]() -> net::awaitable<void> {
                boost::system::error_code ec;
                if (!self->m_watch_stopping) {
                    co_await self->m_http->WatchDisconnect(self->m_watch_cancel.slot(), ec);
                }
                if (ec && ec != net::error::operation_aborted) {
                    self->Cancel();
                } }, [self = shared_from_this()](std::exception_ptr error) {
                if (error) { self->Cancel(); }
                self->m_watch_done.NotifyAll(); });
        }

        /**
         * @brief Complete upstream negotiation and drain the observer before moving the socket.
         */
        auto Connect(
            std::string_view target,
            Headers const& headers,
            std::chrono::steady_clock::duration timeout,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            if (!m_cancelled) {
                try {
                    co_await m_upstream.AsyncConnect(target, headers, timeout, ec);
                } catch (std::exception const&) {
                    Cancel();
                }
            }
            m_watch_stopping = true;
            m_watch_cancel.emit(net::cancellation_type::all);
            static_cast<void>(co_await m_watch_done.Wait());
            m_http->SetAbortHandler({});
            m_http.reset();
            if (m_cancelled) {
                ec = net::error::operation_aborted;
            }
        }

        /**
         * @brief Return owned fields of the successful upstream handshake.
         */
        auto HandshakeHeaders() const -> Headers { return m_upstream.HandshakeHeaders(); }

        /**
         * @brief Start one upstream read loop after the downstream upgrade succeeds.
         */
        auto Open(WebSocketConn::WeakPtr downstream) -> void {
            m_downstream = std::move(downstream);
            m_reader_started = true;
            net::co_spawn(m_executor, [self = shared_from_this()]() -> net::awaitable<void> { co_await self->readUpstream(); }, [self = shared_from_this()](std::exception_ptr error) {
                if (error) {
                    self->Cancel();
                    if (auto downstream{ self->m_downstream.lock() }) {
                        static_cast<void>(downstream->Abort());
                    }
                }
                self->m_reader_finished = true;
                self->m_reader_done.NotifyAll(); });
        }

        /**
         * @brief Apply backpressure by finishing the upstream write before reading another message.
         */
        auto Send(WebSocketMessage const& message) -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await m_upstream.AsyncSend(message, ec);
            if (ec) {
                Cancel();
                if (auto downstream{ m_downstream.lock() }) {
                    co_await downstream->AsyncAbort();
                }
            }
        }

        /**
         * @brief Propagate a downstream close status before final cancellation and drain.
         */
        auto Close(WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
            m_stopping = true;
            if (auto downstream{ weak.lock() }; downstream && !m_cancelled && m_upstream.IsOpen()) {
                auto reason{ downstream->CloseReason() };
                if (valid_ws_close_code(reason.code)) {
                    boost::system::error_code ec;
                    co_await m_upstream.AsyncClose(reason, ec);
                }
            }
        }

        /**
         * @brief Always close the upstream and drain its read loop, including failed accepts.
         */
        auto Finish() -> net::awaitable<void> {
            Cancel();
            co_await m_upstream.AsyncAbort();
            while (m_reader_started && !m_reader_finished) {
                static_cast<void>(co_await m_reader_done.Wait());
            }
        }

    private:
        /**
         * @brief Read one complete message at a time and await each downstream send.
         */
        auto readUpstream() -> net::awaitable<void> {
            while (!m_stopping) {
                WebSocketMessage message;
                boost::system::error_code ec;
                co_await m_upstream.AsyncRead(message, ec);
                auto downstream{ m_downstream.lock() };
                if (m_stopping || !downstream) {
                    co_return;
                }
                if (ec) {
                    if (ec == beast::websocket::error::closed) {
                        auto reason{ m_upstream.CloseReason() };
                        if (!valid_ws_close_code(reason.code)) {
                            reason = WebSocketCloseReason{ 1000, {} };
                        }
                        co_await downstream->AsyncClose(reason, ec);
                    } else {
                        co_await downstream->AsyncAbort();
                    }
                    co_return;
                }
                co_await downstream->AsyncSend(message, ec);
                if (ec) {
                    Cancel();
                    co_return;
                }
            }
        }
    };

    /**
     * @brief Validate one literal-prefix route and prepare each forwarding session before 101.
     */
    class WsForward {
    private:
        std::string m_prefix;     ///< Normalized literal prefix.
        url::UrlInfo m_upstream;  ///< Validated plain WebSocket authority and base path.
        WsForwardConfig m_config; ///< Immutable per-route settings.

    public:
        /**
         * @brief Validate configuration before the server starts.
         */
        WsForward(
            std::string prefix,
            std::string_view upstream,
            WsForwardConfig config
        ) : m_prefix{ proxy_prefix(std::move(prefix)) }, m_config{ config } {
            auto parsed{ url::parse_url(upstream) };
            if (!parsed || !iequals(parsed->scheme, "ws") || parsed->has_query || upstream.find('#') != std::string_view::npos || config.connect_timeout <= std::chrono::steady_clock::duration::zero() || config.message_limit == 0) {
                throw std::invalid_argument{ "mhttp: WebSocket forwarding requires a ws:// upstream and positive limits" };
            }
            m_upstream = std::move(*parsed);
            while (!m_upstream.path.empty() && m_upstream.path.back() == '/') {
                m_upstream.path.pop_back();
            }
        }

        /**
         * @brief Return the normalized prefix for route registration.
         */
        auto Prefix() const -> std::string const& { return m_prefix; }

        /**
         * @brief Open the upstream first; errors remain ordinary downstream HTTP responses.
         */
        auto Prepare(Request& request) -> net::awaitable<std::expected<WsHandlerEntry, Status>> {
            if (auto const status{ validateHandshake(request) }; status != Status::switching_protocols) {
                co_return std::unexpected{ status };
            }
            auto state{ std::make_shared<WsForwardState>(get_impl(request).GetConnection(), m_upstream, m_config.message_limit) };
            auto headers{ proxy_headers(request.Base()) };
            for (auto const field : { Field::sec_websocket_key, Field::sec_websocket_accept, Field::sec_websocket_version, Field::sec_websocket_extensions, Field::content_length, Field::expect }) {
                headers.Erase(field);
            }
            headers.Erase("Forwarded");
            headers.Set(Field::host, m_config.preserve_host ? std::string{ request[Field::host] } : url::make_host_value(m_upstream.host, m_upstream.EffectivePort(), url::Scheme::Plain));
            headers.Set("X-Forwarded-For", request.GetClientIp().to_string());
            headers.Set("X-Forwarded-Host", request[Field::host]);
            headers.Set("X-Forwarded-Proto", "http");
            headers.Set("X-Forwarded-Port", std::to_string(request.LocalEndpoint().port()));
            auto const target{ proxy_target(request, m_prefix, m_upstream.path, m_config.strip_prefix) };
            state->Watch();
            boost::system::error_code ec;
            co_await state->Connect(target, headers, m_config.connect_timeout, ec);
            auto response{ state->HandshakeHeaders() };
            if (ec || !validNegotiation(headers, response)) {
                co_await state->Finish();
                co_return std::unexpected{ ec == net::error::timed_out || ec == beast::error::timeout ? Status::gateway_timeout : Status::bad_gateway };
            }
            WsHandlerEntry handlers;
            handlers.open = [state](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                state->Open(std::move(weak));
                co_return;
            };
            handlers.message = [state](WebSocketConn::WeakPtr, WebSocketMessage message) -> net::awaitable<void> {
                co_await state->Send(message);
            };
            handlers.close = [state](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                co_await state->Close(std::move(weak));
            };
            handlers.abort = [state] {
                state->Cancel();
            };
            handlers.finish = [state]() -> net::awaitable<void> {
                co_await state->Finish();
            };
            handlers.message_limit = m_config.message_limit;
            handlers.response_headers = proxy_headers(response);
            for (auto const field : { Field::sec_websocket_accept, Field::sec_websocket_key, Field::sec_websocket_version, Field::sec_websocket_extensions, Field::content_length }) {
                handlers.response_headers.Erase(field);
            }
            co_return handlers;
        }

    private:
        /**
         * @brief Reject malformed downstream keys and unsupported versions before dialing upstream.
         */
        static auto validateHandshake(Request const& request) -> Status {
            auto const headers{ request.Base() };
            auto const key{ request[Field::sec_websocket_key] };
            if (headers.Count(Field::host) != 1 || request[Field::host].empty() || headers.Count(Field::sec_websocket_key) != 1 || key.size() != 24 || !key.ends_with("==")) {
                return Status::bad_request;
            }
            auto const base64{ [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/'; } };
            if (!std::ranges::all_of(key.substr(0, 22), base64) || std::string_view{ "AQgw" }.find(key[21]) == std::string_view::npos) {
                return Status::bad_request;
            }
            if (headers.Count(Field::sec_websocket_version) != 1 || request[Field::sec_websocket_version] != "13") {
                return Status::upgrade_required;
            }
            return Status::switching_protocols;
        }

        /**
         * @brief Only accept an offered subprotocol and extensions supported by both hops.
         */
        static auto validNegotiation(Headers const& offered, Headers const& response) -> bool {
            if (response.Count(Field::sec_websocket_extensions) != 0 || response.Count(Field::sec_websocket_protocol) > 1) {
                return false;
            }
            auto const selected{ response[Field::sec_websocket_protocol] };
            if (selected.empty()) {
                return response.Count(Field::sec_websocket_protocol) == 0;
            }
            if (proxy_headers(response)[Field::sec_websocket_protocol] != selected) {
                return false;
            }
            for (auto const header : offered) {
                if (header.name == Field::sec_websocket_protocol) {
                    for (auto const token : split(header.value, ",")) {
                        if (trim(token) == selected) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }
    };

} // namespace mhttp::detail
