/**
 * @file test_tls_proxy.cpp
 * @brief Verify HTTPS/WSS upstreams, mixed backend groups, and encrypted CONNECT tunnels.
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
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import mhttp;

#include "support.hpp"
#include "tls_support.hpp"

using namespace test_support;
using mhttp::Field;
using mhttp::Status;
using mhttp::Verb;
using mhttp::WebSocketConn;
using mhttp::WebSocketMessage;
using mhttp::client::HttpClient;

namespace {

    /**
     * @brief Run one bounded proxy scenario.
     */
    auto run_client(std::function<net::awaitable<void>(net::io_context&)> body) -> void {
        net::io_context ioc;
        net::steady_timer watchdog{ ioc, std::chrono::seconds{ 10 } };
        bool finished{};
        watchdog.async_wait([&](boost::system::error_code ec) {
            if (!ec) {
                check(false, "TLS proxy scenario exceeded its deadline");
                ioc.stop();
            }
        });
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true;
            watchdog.cancel(); }, net::detached);
        ioc.run();
        check(finished, "TLS proxy scenario must finish");
    }

    /**
     * @brief Verify HTTPS upstream streaming and metadata over both downstream transports.
     */
    auto test_https_upstream() -> void {
        TlsIdentity identity;
        auto trust{ identity.ClientContext() };
        TestServer upstream{ [&](mhttp::HttpServer& server) {
            server.SetTlsContext(identity.ServerContext());
            server.GetRouter().SetHttpHandler(Verb::get, "/*", [](mhttp::Request& request, mhttp::Response& response) {
                response.Set("X-Proto", request["X-Forwarded-Proto"]);
                response.Set("X-TLS", request.IsTls() ? "yes" : "no");
                response.Set("X-Host", request[Field::host]);
                response.SetStringContent(std::string{ request.Target() }, "text/plain");
            });
            server.GetRouter().SetHttpHandler(Verb::post, "/*", [](mhttp::Request& request, mhttp::Response& response) {
                response.SetStringContent(request.AsString(), "text/plain");
            });
        } };
        auto const origin{ std::format("https://127.0.0.1:{}/base", upstream.Endpoint().port()) };
        for (bool const encrypted : { false, true }) {
            TestServer proxy{ [&](mhttp::HttpServer& server) {
                if (encrypted) {
                    server.SetTlsContext(identity.ServerContext());
                }
                server.SetReverseProxy("/api", origin, { .tls_context = trust });
                server.SetReverseProxy("/untrusted", origin);
            } };
            run_client([&](net::io_context& ioc) -> net::awaitable<void> {
                HttpClient client{ ioc, "127.0.0.1", proxy.Endpoint().port(), encrypted ? mhttp::url::Scheme::Tls : mhttp::url::Scheme::Plain };
                client.SetTlsContext(trust);
                auto first{ co_await client.AsyncGet("/api/one?q=1") };
                check(first && first->AsString() == "/base/one?q=1" && (*first)["X-TLS"] == "yes", "the proxy must rewrite paths and encrypt upstream HTTP");
                check(first && (*first)["X-Proto"] == (encrypted ? "https" : "http") && (*first)["X-Host"] == std::format("127.0.0.1:{}", upstream.Endpoint().port()), "forwarded metadata and Host must describe their respective connections");
                std::string const payload(256 * 1024, 'p');
                auto second{ co_await client.AsyncPost("/api/echo", payload, "application/octet-stream") };
                check(second && second->AsString() == payload, "HTTPS relays must stream large bodies and preserve downstream reuse");
                auto rejected{ co_await client.AsyncGet("/untrusted/") };
                check(rejected && rejected->Result() == Status::bad_gateway, "an untrusted HTTPS upstream must produce 502");
                co_await client.AsyncClose();
            });
        }
    }

    /**
     * @brief Balance between plain and encrypted origins without merging their transport keys.
     */
    auto test_mixed_backends() -> void {
        TlsIdentity identity;
        TestServer plain{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/", [](mhttp::Request&, mhttp::Response& response) { response.SetStringContent("plain", "text/plain"); });
        } };
        TestServer secure{ [&](mhttp::HttpServer& server) {
            server.SetTlsContext(identity.ServerContext());
            server.GetRouter().SetHttpHandler(Verb::get, "/", [](mhttp::Request&, mhttp::Response& response) { response.SetStringContent("secure", "text/plain"); });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            server.SetReverseProxy("/", std::vector<mhttp::UpstreamBackend>{ { std::format("http://127.0.0.1:{}", plain.Endpoint().port()) }, { std::format("https://127.0.0.1:{}", secure.Endpoint().port()) } }, { .tls_context = identity.ClientContext() });
            server.SetReverseProxy("/recovery", std::vector<mhttp::UpstreamBackend>{ { std::format("https://127.0.0.1:{}", secure.Endpoint().port()) }, { std::format("http://127.0.0.1:{}", plain.Endpoint().port()) } });
        } };
        Client client{ proxy.Endpoint() };
        for (int index{}; index < 4; ++index) {
            auto result{ client.Get("/") };
            check(result && result->body() == (index % 2 == 0 ? "plain" : "secure"), "round robin must support mixed HTTP and HTTPS backends");
        }
        {
            Client rejected{ proxy.Endpoint() };
            auto result{ rejected.Get("/recovery") };
            check(result && result->result() == http::status::bad_gateway, "a group must report its selected backend's TLS failure without replay");
        }
        for (int index{}; index < 2; ++index) {
            Client healthy{ proxy.Endpoint() };
            auto result{ healthy.Get("/recovery") };
            check(result && result->body() == "plain", "certificate failures must cool an HTTPS backend while healthy peers remain usable");
        }
    }

    /**
     * @brief Relay WSS messages over either downstream transport and gate untrusted upgrades.
     */
    auto test_wss_upstream() -> void {
        TlsIdentity identity;
        auto trust{ identity.ClientContext() };
        TestServer upstream{ [&](mhttp::HttpServer& server) {
            server.SetTlsContext(identity.ServerContext());
            server.GetRouter().SetWsHandler("/*", [](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                if (auto peer{ weak.lock() }) {
                    boost::system::error_code ec;
                    auto const& request{ peer->HttpRequest() };
                    co_await peer->AsyncSend(WebSocketMessage{ std::format("{}|{}|{}", request.Target(), request["X-Forwarded-Proto"], request.IsTls() ? "tls" : "plain"), false }, ec);
                } }, [](WebSocketConn::WeakPtr weak, WebSocketMessage message) -> net::awaitable<void> {
                if (auto peer{ weak.lock() }) {
                    boost::system::error_code ec;
                    co_await peer->AsyncSend(message, ec);
                } }, [](WebSocketConn::WeakPtr) {});
        } };
        auto const origin{ std::format("wss://127.0.0.1:{}/base", upstream.Endpoint().port()) };
        for (bool const encrypted : { false, true }) {
            TestServer proxy{ [&](mhttp::HttpServer& server) {
                if (encrypted) {
                    server.SetTlsContext(identity.ServerContext());
                }
                server.SetWsForward("/ws", origin, { .tls_context = trust });
                server.SetWsForward("/denied", std::vector<mhttp::UpstreamBackend>{ { origin } });
            } };
            run_client([&](net::io_context& ioc) -> net::awaitable<void> {
                auto const transport{ encrypted ? mhttp::url::Scheme::Tls : mhttp::url::Scheme::Plain };
                mhttp::client::WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port(), transport };
                client.SetTlsContext(trust);
                boost::system::error_code ec;
                co_await client.AsyncConnect("/ws/chat", {}, std::chrono::seconds{ 5 }, ec);
                if (!check(!ec, std::format("WSS upstream must negotiate before downstream 101: {}", ec.message()))) {
                    co_return;
                }
                WebSocketMessage message;
                co_await client.AsyncRead(message, ec);
                check(!ec && message.View() == std::format("/base/chat|{}|tls", encrypted ? "https" : "http"), "WSS relays must preserve paths and trusted forwarding metadata");
                std::string const payload(128 * 1024, 'b');
                co_await client.AsyncSend(WebSocketMessage{ payload, true }, ec);
                co_await client.AsyncRead(message, ec);
                check(!ec && message.IsBinary() && message.View() == payload, "encrypted WebSocket relays must preserve binary messages and backpressure");
                co_await client.AsyncClose({ 1000, "done" }, ec);
                check(!ec, "WSS forwarding must complete both close handshakes");
                HttpClient denied{ ioc, "127.0.0.1", proxy.Endpoint().port(), transport };
                denied.SetTlsContext(trust);
                mhttp::client::Request upgrade{ Verb::get, "/denied" };
                upgrade.Set(Field::connection, "Upgrade");
                upgrade.Set(Field::upgrade, "websocket");
                upgrade.Set(Field::sec_websocket_version, "13");
                upgrade.Set(Field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
                auto rejected{ co_await denied.AsyncSendRequest(upgrade) };
                check(rejected && rejected->Result() == Status::bad_gateway, "untrusted WSS must return 502 before downstream upgrade");
                rejected = co_await denied.AsyncSendRequest(upgrade);
                check(rejected && rejected->Result() == Status::service_unavailable, "a WSS backend in TLS failure cooldown must not accept another upgrade");
                co_await denied.AsyncClose();
            });
        }
    }

    /**
     * @brief Establish and exchange tunnel bytes through an authenticated HTTPS proxy.
     */
    auto test_https_connect() -> void {
        TlsIdentity identity;
        RawTlsServer proxy{ identity };
        auto trust{ identity.ClientContext() };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            mhttp::client::ProxyClient client{ ioc, "127.0.0.1", proxy.Port(), mhttp::url::Scheme::Tls };
            boost::system::error_code ec;
            co_await client.AsyncConnect("target.example:443", {}, ec);
            check(ec && !client.IsOpen(), "CONNECT must verify the HTTPS proxy before sending the tunnel request");
            client.SetTlsContext(trust);
            co_await client.AsyncConnect("target.example:443", {}, ec);
            if (!check(!ec && client.IsOpen(), "a trusted HTTPS proxy must establish CONNECT")) {
                co_return;
            }
            std::array<char, 5> greeting{};
            std::size_t total{};
            while (total < greeting.size() && !ec) {
                total += co_await client.AsyncReadSome(net::buffer(greeting.data() + total, greeting.size() - total), ec);
            }
            check(!ec && std::string_view{ greeting.data(), total } == "ready", "CONNECT must retain tunnel bytes following the encrypted response");
            std::string const payload(32 * 1024, 't');
            co_await client.AsyncWrite(net::buffer(payload), ec);
            std::string received(payload.size(), '\0');
            total = 0;
            while (total < received.size() && !ec) {
                total += co_await client.AsyncReadSome(net::buffer(received.data() + total, received.size() - total), ec);
            }
            check(!ec && received == payload, "CONNECT traffic must retain TLS encryption to the proxy");
            co_await client.AsyncClose();
        });
    }

    /**
     * @brief Wait briefly for an independent peer to observe a wire event.
     */
    auto wait_until(std::function<bool()> predicate) -> net::awaitable<bool> {
        net::steady_timer timer{ co_await net::this_coro::executor };
        for (int attempt{}; attempt < 200; ++attempt) {
            if (predicate()) {
                co_return true;
            }
            timer.expires_after(std::chrono::milliseconds{ 5 });
            co_await timer.async_wait(net::use_awaitable);
        }
        co_return predicate();
    }

    /**
     * @brief Decode a graceful downstream TLS close and promptly cancel a waiting upstream.
     */
    auto test_encrypted_downstream_cancel() -> void {
        TlsIdentity identity;
        RawTlsServer upstream{ identity };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            server.SetTlsContext(identity.ServerContext());
            server.SetReverseProxy("/api", std::format("https://127.0.0.1:{}", upstream.Port()), { .tls_context = identity.ClientContext() });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            net::ssl::context context{ net::ssl::context::tls_client };
            context.add_certificate_authority(net::buffer(identity.certificate));
            net::ssl::stream<tcp::socket> stream{ ioc, context };
            stream.set_verify_mode(net::ssl::verify_peer);
            stream.set_verify_callback(net::ssl::host_name_verification{ "127.0.0.1" });
            boost::system::error_code ec;
            co_await stream.next_layer().async_connect(proxy.Endpoint(), net::redirect_error(net::use_awaitable, ec));
            co_await stream.async_handshake(net::ssl::stream_base::client, net::redirect_error(net::use_awaitable, ec));
            HttpRequest request{ http::verb::get, "/api/hold", 11 };
            request.set(http::field::host, "localhost");
            co_await http::async_write(stream, request, net::redirect_error(net::use_awaitable, ec));
            if (check(!ec && co_await wait_until([&] { return upstream.SawHold(); }), "upstream must receive the held request")) {
                co_await stream.async_shutdown(net::redirect_error(net::use_awaitable, ec));
                check(co_await wait_until([&] { return upstream.SawDisconnect(); }), "downstream close_notify must cancel upstream I/O before the proxy timeout");
            }
        });
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("HTTPS upstream and downstream transports", test_https_upstream);
    run_case("mixed HTTP and HTTPS backends", test_mixed_backends);
    run_case("WSS upstream and downstream transports", test_wss_upstream);
    run_case("CONNECT over HTTPS", test_https_connect);
    run_case("encrypted downstream cancellation", test_encrypted_downstream_cancel);
    return finish("test_tls_proxy");
}
