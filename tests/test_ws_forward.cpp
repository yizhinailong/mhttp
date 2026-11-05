/**
 * @file test_ws_forward.cpp
 * @brief Exercise real WebSocket proxy handshakes, relays, close status, and cancellation.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

import std;
import mhttp;

#include "support.hpp"

using namespace test_support;
using mhttp::Field;
using mhttp::Status;
using mhttp::Verb;
using mhttp::WebSocketCloseReason;
using mhttp::WebSocketConn;
using mhttp::WebSocketMessage;
using mhttp::client::WsClient;

namespace {

    namespace websocket = boost::beast::websocket;

    /**
     * @brief Yield while either independent server executor advances.
     */
    auto delay(std::chrono::milliseconds duration) -> net::awaitable<void> {
        net::steady_timer timer{ co_await net::this_coro::executor, duration };
        co_await timer.async_wait(net::use_awaitable);
    }

    /**
     * @brief Bound asynchronous client exchanges so lost messages fail the test.
     */
    auto run_client(std::function<net::awaitable<void>(net::io_context&)> const& body) -> void {
        net::io_context ioc;
        net::steady_timer watchdog{ ioc, std::chrono::seconds{ 8 } };
        bool finished{ false };
        watchdog.async_wait([&](boost::system::error_code ec) { if (!ec) { ioc.stop(); } });
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true;
            watchdog.cancel(); }, net::detached);
        ioc.run();
        check(finished, "the forwarding client must finish before its watchdog expires");
    }

    /**
     * @brief Build a loopback WebSocket origin.
     */
    auto origin(TestServer const& server) -> std::string {
        return std::format("ws://127.0.0.1:{}", server.Endpoint().port());
    }

    /**
     * @brief Echo complete messages without changing their type.
     */
    auto echo(WebSocketConn::WeakPtr weak, WebSocketMessage message) -> net::awaitable<void> {
        if (auto connection{ weak.lock() }) {
            boost::system::error_code ec;
            co_await connection->AsyncSend(message, ec);
        }
    }

    /**
     * @brief Create an ordinary HTTP upgrade request for pre-101 error assertions.
     */
    auto upgrade(std::string_view target = "/ws") -> HttpRequest {
        return make_request(Verb::get, target, {
        },
                            { { "Connection", "Upgrade" }, { "Upgrade", "websocket" }, { "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==" }, { "Sec-WebSocket-Version", "13" } });
    }

    /**
     * @brief Accept one upstream connection with exact handshake headers, rejection, or a gate.
     */
    class RawUpstream {
    private:
        net::io_context m_ioc; ///< Upstream executor.
        tcp::acceptor m_acceptor{
            m_ioc,
            tcp::endpoint{ net::ip::address_v4::loopback(), 0 }
        };                     ///< Listener.
        std::jthread m_worker; ///< Joined before the executor and acceptor are destroyed.

    public:
        std::atomic<bool> entered{ false }; ///< Request header arrived.
        std::atomic<bool> release{ true };  ///< Gate the upstream response.
        std::atomic<bool> closed{ false };  ///< The proxy closed its upstream socket.

        /**
         * @brief Serve one controllable Beast handshake, or a supplied HTTP response.
         */
        explicit RawUpstream(
            std::string protocol = {},
            std::string rejection = {},
            bool gated = false
        ) {
            release.store(!gated);
            net::co_spawn(m_ioc, [this, protocol = std::move(protocol), rejection = std::move(rejection)]() -> net::awaitable<void> {
                boost::system::error_code ec;
                auto socket{ co_await m_acceptor.async_accept(net::redirect_error(net::use_awaitable, ec)) };
                boost::beast::flat_buffer buffer;
                http::request<http::empty_body> request;
                co_await http::async_read(socket, buffer, request, net::redirect_error(net::use_awaitable, ec));
                entered.store(true);
                socket.non_blocking(true, ec);
                auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 3 } };
                while (!release.load() && std::chrono::steady_clock::now() < deadline) {
                    std::array<char, 1> probe{};
                    static_cast<void>(socket.receive(net::buffer(probe), tcp::socket::message_peek, ec));
                    if (ec && ec != net::error::would_block && ec != net::error::try_again) {
                        closed.store(true);
                        co_return;
                    }
                    ec = {};
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
                if (!ec && !rejection.empty()) {
                    co_await net::async_write(socket, net::buffer(rejection), net::redirect_error(net::use_awaitable, ec));
                } else if (!ec) {
                    websocket::stream<tcp::socket> stream{ std::move(socket) };
                    stream.set_option(websocket::stream_base::decorator([protocol](websocket::response_type& response) {
                        if (!protocol.empty()) { response.set(Field::sec_websocket_protocol, protocol); }
                        response.insert(Field::set_cookie, "a=1");
                        response.insert(Field::set_cookie, "b=2");
                        response.set(Field::connection, "Upgrade, X-Private");
                        response.set("X-Private", "secret");
                    }));
                    co_await stream.async_accept(request, net::redirect_error(net::use_awaitable, ec));
                    if (!ec) {
                        co_await stream.async_read(buffer, net::redirect_error(net::use_awaitable, ec));
                    }
                }
                closed.store(true); }, net::detached);
            m_worker = std::jthread{ [this] { m_ioc.run(); } };
        }

        /**
         * @brief Stop an uncontacted listener or gated handshake and join before flags die.
         */
        ~RawUpstream() {
            m_ioc.stop();
            m_worker.join();
        }

        /**
         * @brief Return the selected ephemeral origin.
         */
        auto Url() const -> std::string {
            return std::format("ws://127.0.0.1:{}", m_acceptor.local_endpoint().port());
        }
    };

    /**
     * @brief Preserve encoded paths, queries, metadata, large headers, and message types.
     */
    auto test_routes_and_messages() -> void {
        TestServer upstream{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetWsHandler("/*", [](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                auto connection{ weak.lock() };
                auto& request{ connection->HttpRequest() };
                auto const greeting{ std::format("{}|{}|{}|{}|{}|{}|{}|{}", request.Target(), request[Field::host], request[Field::authorization], request[Field::origin], request["X-Forwarded-For"], request["X-Forwarded-Host"], request["X-Hop"], request[Field::sec_websocket_extensions]) };
                boost::system::error_code ec;
                co_await connection->AsyncSend(WebSocketMessage{ greeting, false }, ec); }, echo, [](WebSocketConn::WeakPtr) {});
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            server.SetWsForward("/ws/", origin(upstream) + "/base/");
            mhttp::WsForwardConfig config;
            config.strip_prefix = false;
            config.preserve_host = true;
            server.SetWsForward("/keep", origin(upstream), config);
            server.GetRouter().SetHttpHandler(Verb::get, "/ws/local", [](mhttp::Request&, mhttp::Response& response) {
                response.SetStringContent("ordinary HTTP", "text/plain");
            });
            server.GetRouter().SetWsHandler("/ws/local", [](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                boost::system::error_code ec;
                co_await weak.lock()->AsyncSend(WebSocketMessage{ "local", false }, ec); }, echo, [](WebSocketConn::WeakPtr) {});
        } };
        Client http_client{ proxy.Endpoint() };
        auto const ordinary{ http_client.Get("/ws/local") };
        check(ordinary && ordinary->body() == "ordinary HTTP", "HTTP must coexist with forwarded and local WebSocket routes");
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            for (auto const& [target, path] : std::array<std::pair<std::string_view, std::string_view>, 5>{
                     std::pair{ "/ws/a%2Fb?x=%2F&x=2", "/base/a%2Fb?x=%2F&x=2" },
                     std::pair{         "/%77s/item?",           "/base/item?" },
                     std::pair{                 "/ws",                "/base/" },
                     std::pair{      "/keep/item?q=1",        "/keep/item?q=1" },
                     std::pair{           "/ws/local",                 "local" }
            }) {
                WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
                mhttp::Headers headers;
                headers.Set(Field::host, "public.example");
                headers.Set(Field::authorization, "Bearer token");
                headers.Set(Field::origin, "https://browser.example");
                headers.Set(Field::connection, "Upgrade, X-Hop");
                headers.Set("X-Hop", "secret");
                headers.Set("X-Forwarded-For", "203.0.113.1");
                headers.Set(Field::sec_websocket_extensions, "permessage-deflate");
                headers.Set("X-Large", std::string(12000, 'h'));
                boost::system::error_code ec;
                co_await client.AsyncConnect(target, headers, std::chrono::seconds{ 2 }, ec);
                if (!check(!ec, "the routed upstream must upgrade successfully")) {
                    co_return;
                }
                WebSocketMessage message;
                co_await client.AsyncRead(message, ec);
                auto const expected{ target == "/ws/local" ? std::string{ "local" } : std::format("{}|{}|Bearer token|https://browser.example|127.0.0.1|public.example||", path, target.starts_with("/keep") ? "public.example" : std::format("127.0.0.1:{}", upstream.Endpoint().port())) };
                check(!ec && message.View() == expected, "rewrite, Host, Origin, authorization, and sanitized metadata must reach the upstream");
                co_await client.AsyncPing("alive", ec);
                co_await client.AsyncSend(WebSocketMessage{ "text", false }, ec);
                co_await client.AsyncRead(message, ec);
                check(!ec && message.IsText() && message.View() == "text", "text messages and control frames must remain usable");
                std::string binary(70000, '\0');
                binary.back() = static_cast<char>(255);
                co_await client.AsyncSend(WebSocketMessage{ binary, true }, ec);
                co_await client.AsyncRead(message, ec);
                check(!ec && message.IsBinary() && message.View() == binary, "fragmented binary messages must survive both proxy hops");
                co_await client.AsyncClose(ec);
                check(!ec, "a downstream close must complete");
            }
        });
    }

    /**
     * @brief Do not emit downstream 101 until the upstream has accepted the handshake.
     */
    auto test_handshake_gate_and_headers() -> void {
        RawUpstream upstream{ "chat.v2", {}, true };
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetWsForward("/ws", upstream.Url()); } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            mhttp::Headers headers;
            headers.Insert(Field::sec_websocket_protocol, "chat.v1");
            headers.Insert(Field::sec_websocket_protocol, "chat.v2, chat.v3");
            boost::system::error_code ec;
            bool connected{ false };
            net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                co_await client.AsyncConnect("/ws", headers, std::chrono::seconds{ 2 }, ec);
                connected = true; }, net::detached);
            for (int i{ 0 }; i < 100 && !upstream.entered.load(); ++i) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            co_await delay(std::chrono::milliseconds{ 30 });
            check(upstream.entered.load() && !connected, "the downstream upgrade must wait for upstream acceptance");
            upstream.release.store(true);
            while (!connected) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            check(!ec, "an offered subprotocol must be accepted after the gate opens");
            auto const response{ client.HandshakeHeaders() };
            check(response[Field::sec_websocket_protocol] == "chat.v2" && response.Count(Field::set_cookie) == 2 && response["X-Private"].empty(), "the negotiated protocol and duplicate cookies must survive, while nominated hop headers are removed");
            co_await client.AsyncClose(ec);
        });
    }

    /**
     * @brief Keep rejection, invalid negotiation, and timeout errors before downstream 101.
     */
    auto test_handshake_failures() -> void {
        for (int scenario{ 0 }; scenario < 4; ++scenario) {
            RawUpstream upstream{ scenario == 2 ? "not-offered" : "", scenario == 0 ? "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n" : scenario == 1 ? "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n" :
                                                                                                                                                              "",
                                  scenario == 3 };
            TestServer proxy{ [&](mhttp::HttpServer& server) {
                mhttp::WsForwardConfig config;
                config.connect_timeout = std::chrono::milliseconds{ 80 };
                server.SetWsForward("/ws", upstream.Url(), config);
            } };
            Client client{ proxy.Endpoint() };
            auto request{ upgrade() };
            request.set(Field::sec_websocket_protocol, "chat.v1");
            auto const response{ client.Send(request) };
            check(response && response->result() == (scenario == 3 ? Status::gateway_timeout : Status::bad_gateway), "upstream rejection and invalid protocol must be 502; a handshake timeout must be 504");
            upstream.release.store(true);
        }
        RawUpstream untouched;
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetWsForward("/ws", untouched.Url()); } };
        for (int scenario{ 0 }; scenario < 3; ++scenario) {
            Client client{ proxy.Endpoint() };
            auto request{ upgrade() };
            if (scenario == 0) {
                request.set(Field::sec_websocket_key, "invalid");
            }
            if (scenario == 1) {
                request.set(Field::sec_websocket_version, "12");
            }
            if (scenario == 2) {
                request.body() = "body";
                request.prepare_payload();
            }
            auto const response{ client.Send(request) };
            check(response && response->result() == (scenario == 1 ? Status::upgrade_required : Status::bad_request), "malformed downstream handshakes must fail before contacting upstream");
            if (scenario == 1 && response) {
                check((*response)[Field::sec_websocket_version] == "13", "426 must advertise the supported version");
            }
        }
        check(!untouched.entered.load(), "invalid downstream handshakes must not open an upstream session");
    }

    /**
     * @brief Preserve close codes and reasons in both directions and handle abnormal loss.
     */
    auto test_close_propagation() -> void {
        std::atomic<std::uint16_t> code{ 0 };
        std::atomic<bool> reason_ok{ false };
        TestServer upstream{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetWsHandler("/*", [](WebSocketConn::WeakPtr) {}, [](WebSocketConn::WeakPtr weak, WebSocketMessage message) -> net::awaitable<void> {
                auto connection{ weak.lock() };
                boost::system::error_code ec;
                if (message.View() == "close") { co_await connection->AsyncClose(WebSocketCloseReason{ 4001, "upstream done" }, ec); }
                if (message.View() == "abort") { co_await connection->AsyncAbort(); } }, [&](WebSocketConn::WeakPtr weak) {
                if (auto connection{ weak.lock() }) {
                    auto const reason{ connection->CloseReason() };
                    code.store(reason.code);
                    reason_ok.store(reason.reason == "downstream done");
                } });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetWsForward("/ws", origin(upstream)); } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            for (std::string_view const command : { "close", "abort", "downstream" }) {
                WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
                boost::system::error_code ec;
                co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 2 }, ec);
                if (command == "downstream") {
                    co_await client.AsyncClose(WebSocketCloseReason{ 4002, "downstream done" }, ec);
                    for (int i{ 0 }; i < 100 && code.load() != 4002; ++i) {
                        co_await delay(std::chrono::milliseconds{ 5 });
                    }
                    check(!ec && code.load() == 4002 && reason_ok.load(), "downstream close code and reason must reach the upstream callback");
                } else {
                    co_await client.AsyncSend(WebSocketMessage{ std::string{ command }, false }, ec);
                    WebSocketMessage message;
                    co_await client.AsyncRead(message, ec);
                    if (command == "close") {
                        auto const reason{ client.CloseReason() };
                        check(ec == websocket::error::closed && reason.code == 4001 && reason.reason == "upstream done", "upstream application close status must reach the downstream client");
                    } else {
                        check(static_cast<bool>(ec), "an abnormal upstream loss must terminate downstream reads");
                    }
                }
            }
        });
    }

    /**
     * @brief Retain an early masked frame in the same TCP write as the upgrade header.
     */
    auto test_prefetched_frame() -> void {
        TestServer upstream{ [](mhttp::HttpServer& server) { server.GetRouter().SetWsHandler("/*", [](WebSocketConn::WeakPtr) {}, echo, [](WebSocketConn::WeakPtr) {}); } };
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetWsForward("/ws", origin(upstream)); } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            tcp::socket socket{ ioc };
            boost::system::error_code ec;
            co_await socket.async_connect(proxy.Endpoint(), net::redirect_error(net::use_awaitable, ec));
            std::string sent{ "GET /ws HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" };
            sent += static_cast<char>(0x81);
            sent += static_cast<char>(0x85);
            sent += "abcd";
            for (std::size_t i{ 0 }; i < 5; ++i) {
                sent += static_cast<char>(std::string_view{ "early" }[i] ^ std::string_view{ "abcd" }[i % 4]);
            }
            co_await net::async_write(socket, net::buffer(sent), net::redirect_error(net::use_awaitable, ec));
            boost::beast::flat_buffer buffer;
            http::response_parser<http::empty_body> parser;
            co_await http::async_read_header(socket, buffer, parser, net::redirect_error(net::use_awaitable, ec));
            check(!ec && parser.get().result() == Status::switching_protocols, "prefetched data must survive upstream handshake preparation");
            while (!ec && buffer.size() < 7) {
                auto const size{ co_await socket.async_read_some(buffer.prepare(64), net::redirect_error(net::use_awaitable, ec)) };
                buffer.commit(size);
            }
            auto const wire{ boost::beast::buffers_to_string(buffer.data()) };
            check(!ec && wire.size() >= 7 && wire.substr(2, 5) == "early", "the early frame must be forwarded exactly once");
            socket.close(ec);
        });
    }

    /**
     * @brief Bound complete messages independently on the two incoming proxy hops.
     */
    auto test_message_limits() -> void {
        TestServer upstream{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetWsHandler("/*", [](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                auto connection{ weak.lock() };
                if (connection->HttpRequest().Path() == "/large") {
                    boost::system::error_code ec;
                    co_await connection->AsyncSend(WebSocketMessage{ std::string(17, 'x'), false }, ec);
                } }, echo, [](WebSocketConn::WeakPtr) {});
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::WsForwardConfig config;
            config.message_limit = 16;
            server.SetWsForward("/ws", origin(upstream), config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            for (std::string_view const target : { "/ws", "/ws/large" }) {
                WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
                boost::system::error_code ec;
                co_await client.AsyncConnect(target, {}, std::chrono::seconds{ 2 }, ec);
                if (target == "/ws") {
                    co_await client.AsyncSend(WebSocketMessage{ std::string(16, 'x'), false }, ec);
                    WebSocketMessage accepted;
                    co_await client.AsyncRead(accepted, ec);
                    check(!ec && accepted.View().size() == 16, "the configured message limit must be inclusive");
                    co_await client.AsyncSend(WebSocketMessage{ std::string(17, 'x'), false }, ec);
                }
                WebSocketMessage rejected;
                co_await client.AsyncRead(rejected, ec);
                check(static_cast<bool>(ec), "an oversized message on either hop must end the forwarded session");
            }
        });
    }

    /**
     * @brief Cancel pending handshakes and established reads without waiting for upstream timeout.
     */
    auto test_stop_and_disconnect() -> void {
        for (bool const stop : { false, true }) {
            RawUpstream upstream{ {}, {}, true };
            TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetWsForward("/ws", upstream.Url()); } };
            run_client([&](net::io_context& ioc) -> net::awaitable<void> {
                tcp::socket socket{ ioc };
                boost::system::error_code ec;
                co_await socket.async_connect(proxy.Endpoint(), net::redirect_error(net::use_awaitable, ec));
                std::string const wire{ "GET /ws HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" };
                co_await net::async_write(socket, net::buffer(wire), net::redirect_error(net::use_awaitable, ec));
                for (int i{ 0 }; i < 100 && !upstream.entered.load(); ++i) {
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
                check(upstream.entered.load(), "the upstream handshake must start before cancellation");
                auto const start{ std::chrono::steady_clock::now() };
                if (stop) {
                    co_await proxy.server.AsyncStop();
                } else {
                    socket.close(ec);
                }
                for (int i{ 0 }; i < 100 && !upstream.closed.load(); ++i) {
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
                check(upstream.closed.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds{ 1 }, "stop and downstream EOF must promptly cancel the upstream handshake");
                upstream.release.store(true);
            });
        }
        std::atomic<int> closed{ 0 };
        TestServer upstream{ [&](mhttp::HttpServer& server) { server.GetRouter().SetWsHandler("/*", [](WebSocketConn::WeakPtr) {}, echo, [&](WebSocketConn::WeakPtr) { ++closed; }); } };
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetWsForward("/ws", origin(upstream)); } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 2 }, ec);
            auto const start{ std::chrono::steady_clock::now() };
            co_await proxy.server.AsyncStop();
            WebSocketMessage message;
            co_await client.AsyncRead(message, ec);
            for (int i{ 0 }; i < 100 && closed.load() == 0; ++i) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            check(ec && closed.load() == 1 && std::chrono::steady_clock::now() - start < std::chrono::seconds{ 1 }, "stop must cancel both idle reads and drain the upstream relay");
        });
    }

    /**
     * @brief Exercise simultaneous bursts and stop a relay stalled by a non-reading client.
     */
    auto test_full_duplex_and_backpressure() -> void {
        std::atomic<int> sent{ 0 };
        std::atomic<int> closed{ 0 };
        TestServer upstream{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetWsHandler("/*", [&](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                auto connection{ weak.lock() };
                auto const slow{ connection->HttpRequest().Path() == "/slow" };
                for (int i{ 0 }; i < (slow ? 64 : 12); ++i) {
                    boost::system::error_code ec;
                    co_await connection->AsyncSend(WebSocketMessage{ std::string(slow ? 1024 * 1024 : 128 * 1024, static_cast<char>('A' + i % 26)), true }, ec);
                    if (ec) { break; }
                    if (slow) { ++sent; }
                } }, echo, [&](WebSocketConn::WeakPtr) { ++closed; });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) { server.SetWsForward("/ws", origin(upstream)); } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", {}, std::chrono::seconds{ 2 }, ec);
            bool sender_done{ false };
            boost::system::error_code sender_error;
            net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                for (int i{ 0 }; i < 12 && !sender_error; ++i) {
                    co_await client.AsyncSend(WebSocketMessage{ std::string(128 * 1024, static_cast<char>('a' + i)), true }, sender_error);
                }
                sender_done = true; }, net::detached);
            for (int i{ 0 }; i < 24; ++i) {
                WebSocketMessage message;
                co_await client.AsyncRead(message, ec);
                auto const expected{ static_cast<char>(i < 12 ? 'A' + i : 'a' + i - 12) };
                check(!ec && message.IsBinary() && message.View() == std::string(128 * 1024, expected), "both directions must preserve ordered bursts while the opposite direction is busy");
                if (ec) {
                    break;
                }
            }
            while (!sender_done) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            check(!sender_error, "concurrent upstream sends must complete");
            co_await client.AsyncClose(ec);
            for (int i{ 0 }; i < 100 && closed.load() == 0; ++i) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            co_await client.AsyncConnect("/ws/slow", {}, std::chrono::seconds{ 2 }, ec);
            check(!ec, "the slow receiver must connect");
            co_await delay(std::chrono::milliseconds{ 80 });
            check(sent.load() < 64, "a non-reading downstream must apply backpressure to the upstream sender");
            auto const start{ std::chrono::steady_clock::now() };
            co_await proxy.server.AsyncStop();
            for (int i{ 0 }; i < 100 && closed.load() < 2; ++i) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            check(closed.load() == 2 && std::chrono::steady_clock::now() - start < std::chrono::seconds{ 1 }, "stop must drain a blocked downstream write and release the upstream sender");
            co_await client.AsyncAbort();
        });
    }

    /**
     * @brief Reject unsupported transports and nonsensical route settings at registration.
     */
    auto test_configuration() -> void {
        net::io_context ioc;
        mhttp::HttpServer server{ ioc };
        for (auto const& [prefix, upstream] : std::array<std::pair<std::string_view, std::string_view>, 5>{
                 std::pair{ "/ws/:id",          "ws://localhost" },
                 std::pair{     "/ws",       "https://localhost" },
                 std::pair{     "/ws",        "http://localhost" },
                 std::pair{     "/ws",      "ws://localhost?q=1" },
                 std::pair{     "/ws", "ws://localhost#fragment" }
        }) {
            bool rejected{ false };
            try {
                server.SetWsForward(std::string{ prefix }, upstream);
            } catch (std::invalid_argument const&) {
                rejected = true;
            }
            check(rejected, "unsupported upstreams and nonliteral prefixes must fail registration");
        }
        mhttp::WsForwardConfig config;
        config.message_limit = 0;
        bool rejected{ false };
        try {
            server.SetWsForward("/ws", "ws://localhost", config);
        } catch (std::invalid_argument const&) {
            rejected = true;
        }
        check(rejected, "a forwarding route must have a positive message bound");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("routes and bidirectional messages", test_routes_and_messages);
    run_case("upstream handshake gate and headers", test_handshake_gate_and_headers);
    run_case("handshake failures and rejection", test_handshake_failures);
    run_case("close propagation", test_close_propagation);
    run_case("prefetched frame", test_prefetched_frame);
    run_case("message limits", test_message_limits);
    run_case("stop and disconnect", test_stop_and_disconnect);
    run_case("full duplex and backpressure", test_full_duplex_and_backpressure);
    run_case("configuration", test_configuration);
    return finish("test_ws_forward");
}
