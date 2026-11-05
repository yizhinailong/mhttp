/**
 * @file test_wss.cpp
 * @brief Verify encrypted WebSocket messages, trust, close handshakes, and cancellation.
 */
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

import std;
import mhttp;

#include "support.hpp"
#include "tls_support.hpp"

using namespace test_support;
using mhttp::WebSocketConn;
using mhttp::WebSocketMessage;
using mhttp::client::WsClient;

namespace {

    /**
     * @brief Run a bounded WebSocket scenario on its own context.
     */
    auto run_client(std::function<net::awaitable<void>(net::io_context&)> body) -> void {
        net::io_context ioc;
        net::steady_timer watchdog{ ioc, std::chrono::seconds{ 10 } };
        bool finished{};
        watchdog.async_wait([&](boost::system::error_code ec) {
            if (!ec) {
                check(false, "WSS scenario exceeded its deadline");
                ioc.stop();
            }
        });
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true;
            watchdog.cancel(); }, net::detached);
        ioc.run();
        check(finished, "WSS scenario must finish");
    }

    /**
     * @brief Echo complete messages while preserving their binary/text flag.
     */
    auto echo(WebSocketConn::WeakPtr weak, WebSocketMessage message) -> net::awaitable<void> {
        if (auto peer{ weak.lock() }) {
            boost::system::error_code ec;
            co_await peer->AsyncSend(message, ec);
        }
    }

    /**
     * @brief Round-trip TLS records and verify peer close status and server request metadata.
     */
    auto test_wss_round_trip() -> void {
        TlsIdentity identity;
        auto trust{ identity.ClientContext() };
        std::atomic<unsigned> close_code{};
        TestServer server{ [&](mhttp::HttpServer& listener) {
            listener.SetTlsContext(identity.ServerContext());
            listener.GetRouter().SetWsHandler("/ws", [](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                if (auto peer{ weak.lock() }) {
                    boost::system::error_code ec;
                    co_await peer->AsyncSend(WebSocketMessage{ peer->HttpRequest().IsTls() ? "tls" : "plain", false }, ec);
                } }, echo, [&](WebSocketConn::WeakPtr weak) {
                if (auto peer{ weak.lock() }) {
                    close_code.store(peer->CloseReason().code);
                } });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "localhost", server.Endpoint().port(), mhttp::url::Scheme::Tls };
            client.SetTlsContext(trust);
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 5 }, ec);
            if (!check(!ec && client.IsOpen(), std::format("WSS handshake must succeed: {}", ec.message()))) {
                co_return;
            }
            WebSocketMessage message;
            co_await client.AsyncRead(message, ec);
            check(!ec && message.View() == "tls", "an encrypted upgrade must retain request transport metadata");
            std::string const payload(256 * 1024, 'w');
            co_await client.AsyncSend(WebSocketMessage{ payload, true }, ec);
            co_await client.AsyncRead(message, ec);
            check(!ec && message.IsBinary() && message.View() == payload, "binary messages must span TLS records without corruption");
            co_await client.AsyncPing("probe", ec);
            check(!ec, "WSS control frames must be writable");
            co_await client.AsyncSend(WebSocketMessage{ "text", false }, ec);
            co_await client.AsyncRead(message, ec);
            check(!ec && !message.IsBinary() && message.View() == "text", "WSS text messages must survive intervening control frames");
            co_await client.AsyncClose({ 1000, "done" }, ec);
            check(!ec && !client.IsOpen(), std::format("WSS close and TLS teardown must complete: {}", ec.message()));
        });
        server.server.Stop().get();
        check(close_code.load() == 1000, "server callbacks must observe the peer's close status");
    }

    /**
     * @brief Reject bad trust and host names before the HTTP upgrade.
     */
    auto test_wss_verification() -> void {
        TlsIdentity identity{ "DNS:another.example" };
        TestServer server{ [&](mhttp::HttpServer& listener) {
            listener.SetTlsContext(identity.ServerContext());
            listener.GetRouter().SetWsHandler("/ws", [](WebSocketConn::WeakPtr) {}, echo, [](WebSocketConn::WeakPtr) {});
        } };
        auto trust{ identity.ClientContext() };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Endpoint().port(), mhttp::url::Scheme::Tls };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 5 }, ec);
            check(ec && !client.IsOpen(), "WSS must reject an untrusted certificate");
            client.SetTlsContext(trust);
            co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 5 }, ec);
            check(ec && !client.IsOpen(), "WSS must reject a trusted certificate for another host");
        });
    }

    /**
     * @brief Stop an encrypted session whose server receive loop is waiting for a frame.
     */
    auto test_wss_stop() -> void {
        TlsIdentity identity;
        auto trust{ identity.ClientContext() };
        TestServer server{ [&](mhttp::HttpServer& listener) {
            listener.SetTlsContext(identity.ServerContext());
            listener.GetRouter().SetWsHandler("/ws", [](WebSocketConn::WeakPtr) {}, echo, [](WebSocketConn::WeakPtr) {});
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Endpoint().port(), mhttp::url::Scheme::Tls };
            client.SetTlsContext(trust);
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 5 }, ec);
            if (!check(!ec, "WSS must connect before stop")) {
                co_return;
            }
            co_await server.server.AsyncStop();
            WebSocketMessage message;
            co_await client.AsyncRead(message, ec);
            check(ec && !client.IsOpen(), "server stop must close and drain an idle WSS session");
        });
    }

    /**
     * @brief Bound and abort TLS negotiation before starting the WebSocket upgrade.
     */
    auto test_wss_deadline_and_cancel() -> void {
        net::io_context peer_ioc;
        tcp::acceptor listener{
            peer_ioc,
            { tcp::v4(), 0 }
        };
        tcp::socket stalled{ peer_ioc };
        listener.async_accept(stalled, [](boost::system::error_code) {});
        std::jthread worker{ [&] { peer_ioc.run(); } };
        auto insecure{ mhttp::TlsClientContext::Create({ .verify_peer = false }) };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", listener.local_endpoint().port(), mhttp::url::Scheme::Tls };
            client.SetTlsContext(*insecure);
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", {}, std::chrono::milliseconds{ 100 }, ec);
            check(ec == boost::beast::error::timeout && !client.IsOpen(), "the WSS connect deadline must include TLS negotiation");
            net::steady_timer cancel{ ioc, std::chrono::milliseconds{ 50 } };
            cancel.async_wait([&](boost::system::error_code timer_ec) {
                if (!timer_ec) {
                    static_cast<void>(client.Abort());
                }
            });
            co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 5 }, ec);
            check(ec == net::error::operation_aborted && !client.IsOpen(), "abort must cancel a WSS handshake");
        });
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("WSS messages and close handshake", test_wss_round_trip);
    run_case("WSS certificate and host verification", test_wss_verification);
    run_case("WSS handshake deadline and cancellation", test_wss_deadline_and_cancel);
    run_case("WSS server stop", test_wss_stop);
    return finish("test_wss");
}
