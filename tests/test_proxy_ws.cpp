/**
 * @file test_proxy_ws.cpp
 * @brief Exercise ProxyClient through a CONNECT proxy and WsClient against a WebSocket echo server.
 */
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
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
using mhttp::WebSocketMessage;
using mhttp::client::ProxyClient;
using mhttp::client::WsClient;

namespace {

    namespace websocket = boost::beast::websocket;

    auto run_client(std::function<net::awaitable<void>(net::io_context&)> const& body) -> void {
        net::io_context ioc;
        bool finished{ false };
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true; }, net::detached);
        ioc.run();
        check(finished, "the client coroutine must run to completion");
    }

    auto sleep_for(std::chrono::milliseconds duration) -> net::awaitable<void> {
        net::steady_timer timer{ co_await net::this_coro::executor, duration };
        co_await timer.async_wait(net::use_awaitable);
    }

    /**
     * @brief Synchronous CONNECT proxy serving a fixed number of connections on its own thread.
     */
    struct TunnelProxy {
        net::io_context ioc; ///< Context for sockets.
        tcp::acceptor acceptor{
            ioc,
            tcp::endpoint{ net::ip::make_address("127.0.0.1"), 0 }
        };                                 ///< Listening socket.
        std::string greeting;              ///< Bytes sent right after 200.
        unsigned reject_status{ 0 };       ///< Refusal status; 0 = accept.
        std::mutex mutex;                  ///< Guards `requests`.
        std::vector<std::string> requests; ///< "METHOD target|Proxy-Authorization".
        std::jthread worker;               ///< Accept loop.

        TunnelProxy(int connections, std::string greeting_bytes, unsigned reject = 0) : greeting{ std::move(greeting_bytes) }, reject_status{ reject } {
            worker = std::jthread{ [this, connections] {
                for (int i{ 0 }; i < connections; ++i) {
                    boost::system::error_code ec;
                    tcp::socket client{ ioc };
                    acceptor.accept(client, ec);
                    if (ec) {
                        return;
                    }
                    handle(client);
                }
            } };
        }

        TunnelProxy(TunnelProxy const&) = delete;
        auto operator=(TunnelProxy const&) -> TunnelProxy& = delete;

        ~TunnelProxy() {
            boost::system::error_code ec;
            acceptor.close(ec);
        }

        [[nodiscard]] auto Port() const -> std::uint16_t {
            return acceptor.local_endpoint().port();
        }

        auto Requests() -> std::vector<std::string> {
            std::lock_guard lock{ mutex };
            return requests;
        }

        auto handle(tcp::socket& client) -> void {
            boost::system::error_code ec;
            boost::beast::flat_buffer buffer;
            http::request_parser<http::empty_body> parser;
            http::read_header(client, buffer, parser, ec);
            if (ec) {
                return;
            }
            auto const& request{ parser.get() };
            std::string const target{ request.target() };
            {
                std::lock_guard lock{ mutex };
                requests.push_back(std::format("{} {}|{}", std::string_view{ request.method_string() }, target, std::string_view{ request[http::field::proxy_authorization] }));
            }
            if (reject_status != 0) {
                net::write(client, net::buffer(std::format("HTTP/1.1 {} Refused\r\nContent-Length: 0\r\n\r\n", reject_status)), ec);
                return;
            }
            auto const colon{ target.rfind(':') };
            tcp::socket upstream{ ioc };
            upstream.connect(tcp::endpoint{ net::ip::make_address(target.substr(0, colon)), static_cast<std::uint16_t>(std::stoi(target.substr(colon + 1))) }, ec);
            if (ec) {
                net::write(client, net::buffer(std::string_view{ "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n" }), ec);
                return;
            }
            net::write(client, net::buffer("HTTP/1.1 200 Connection Established\r\n\r\n" + greeting), ec);
            if (buffer.size() > 0) {
                net::write(upstream, buffer.data(), ec);
            }
            auto const pump{ [](tcp::socket& from, tcp::socket& to) {
                std::array<char, 4096> chunk{};
                boost::system::error_code pump_ec;
                for (;;) {
                    auto const n{ from.read_some(net::buffer(chunk), pump_ec) };
                    if (pump_ec) {
                        break;
                    }
                    net::write(to, net::buffer(chunk.data(), n), pump_ec);
                    if (pump_ec) {
                        break;
                    }
                }
                to.shutdown(tcp::socket::shutdown_send, pump_ec);
            } };
            std::jthread up{ [&] { pump(client, upstream); } };
            pump(upstream, client);
        }
    };

    /**
     * @brief Synchronous WebSocket echo server on its own thread.
     *
     * Greets with "hello <X-Token>", echoes every message with its frame type, answers "bye"
     * with a close frame, and declines the upgrade for target "/reject".
     */
    struct WsEchoServer {
        net::io_context ioc; ///< Context for sockets.
        tcp::acceptor acceptor{
            ioc,
            tcp::endpoint{ net::ip::make_address("127.0.0.1"), 0 }
        };                   ///< Listening socket.
        std::jthread worker; ///< Accept loop.

        explicit WsEchoServer(int connections) {
            worker = std::jthread{ [this, connections] {
                for (int i{ 0 }; i < connections; ++i) {
                    boost::system::error_code ec;
                    tcp::socket socket{ ioc };
                    acceptor.accept(socket, ec);
                    if (ec) {
                        return;
                    }
                    handle(std::move(socket));
                }
            } };
        }

        WsEchoServer(WsEchoServer const&) = delete;
        auto operator=(WsEchoServer const&) -> WsEchoServer& = delete;

        ~WsEchoServer() {
            boost::system::error_code ec;
            acceptor.close(ec);
        }

        [[nodiscard]] auto Port() const -> std::uint16_t {
            return acceptor.local_endpoint().port();
        }

        static auto handle(tcp::socket socket) -> void {
            boost::system::error_code ec;
            boost::beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request, ec);
            if (ec) {
                return;
            }
            if (request.target() == "/reject") {
                http::response<http::string_body> refusal{ http::status::forbidden, 11 };
                refusal.prepare_payload();
                http::write(socket, refusal, ec);
                return;
            }
            websocket::stream<tcp::socket> ws{ std::move(socket) };
            ws.accept(request, ec);
            if (ec) {
                return;
            }
            ws.text(true);
            ws.write(net::buffer(std::format("hello {}", std::string_view{ request["X-Token"] })), ec);
            for (;;) {
                boost::beast::flat_buffer message;
                ws.read(message, ec);
                if (ec) {
                    return;
                }
                if (ws.got_text() && boost::beast::buffers_to_string(message.data()) == "bye") {
                    ws.close(websocket::close_code::normal, ec);
                    // Read until the client's close answer arrives.
                    boost::beast::flat_buffer drain;
                    ws.read(drain, ec);
                    return;
                }
                ws.text(ws.got_text());
                ws.write(message.data(), ec);
                if (ec) {
                    return;
                }
            }
        }
    };

    auto test_tunnel() -> void {
        TestServer target{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/hello", [](mhttp::Request&, mhttp::Response& resp) { resp.SetStringContent("through the tunnel", "text/plain"); });
        } };
        TunnelProxy proxy{ 1, "HELLO" };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            ProxyClient tunnel{ ioc, "127.0.0.1", proxy.Port() };
            boost::system::error_code ec;
            std::array<char, 16> scratch{};
            co_await tunnel.AsyncWrite(net::buffer(std::string_view{ "x" }), ec);
            check(ec == mhttp::Error::NotInitialized, "writing before CONNECT must fail with NotInitialized");

            mhttp::Headers headers;
            headers.Set(Field::proxy_authorization, "Basic dXNlcjpwdw==");
            auto const authority{ std::format("127.0.0.1:{}", target.Endpoint().port()) };
            co_await tunnel.AsyncConnect(authority, headers, ec);
            check(!ec && tunnel.IsOpen() && tunnel.HttpStatus() == Status::ok, std::format("CONNECT must open the tunnel: {}", ec.message()));

            co_await tunnel.AsyncWrite(net::buffer(std::string_view{ "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" }), ec);
            check(!ec, "writing into the tunnel must succeed");
            std::string received;
            for (;;) {
                boost::system::error_code read_ec;
                auto const n{ co_await tunnel.AsyncReadSome(net::buffer(scratch), read_ec) };
                received.append(scratch.data(), n);
                if (read_ec) {
                    break;
                }
            }
            // httplib read the CONNECT answer into a local buffer and lost the greeting.
            check(received.starts_with("HELLO"), "bytes sent right after the answer must not be lost");
            check(received.contains("HTTP/1.1 200") && received.ends_with("through the tunnel"), std::format("the target's response must arrive through the tunnel, got '{}'", received));
            check(!tunnel.IsOpen(), "the tunnel must close once the target closed");
        });
        auto const requests{ proxy.Requests() };
        check(requests.size() == 1 && requests[0] == std::format("CONNECT 127.0.0.1:{}|Basic dXNlcjpwdw==", target.Endpoint().port()), "the proxy must receive the CONNECT target and Proxy-Authorization");
    }

    auto test_tunnel_refused() -> void {
        TunnelProxy proxy{ 1, "", 407 };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            ProxyClient tunnel{ ioc, "127.0.0.1", proxy.Port() };
            boost::system::error_code ec;
            co_await tunnel.AsyncConnect("example.com:443", mhttp::Headers{}, ec);
            check(ec == mhttp::Error::HttpStatus && tunnel.HttpStatus() == Status::proxy_authentication_required, "a refused CONNECT must report the proxy's status");
            check(!tunnel.IsOpen(), "a refused CONNECT must leave the tunnel closed");

            ProxyClient tls{ ioc, "127.0.0.1", proxy.Port(), mhttp::url::Scheme::Tls };
            co_await tls.AsyncConnect("example.com:443", mhttp::Headers{}, ec);
            check(ec == net::error::operation_not_supported, "a TLS proxy must fail clearly until TLS is ported");
        });
    }

    auto test_websocket_messages() -> void {
        WsEchoServer server{ 2 };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Port() };
            boost::system::error_code ec;
            WebSocketMessage message;
            co_await client.AsyncRead(message, ec);
            check(ec == mhttp::Error::NotInitialized, "reading before connecting must fail with NotInitialized");

            mhttp::Headers headers;
            headers.Set("X-Token", "abc");
            co_await client.AsyncConnect("/ws", headers, std::chrono::seconds{ 5 }, ec);
            check(!ec && client.IsOpen(), std::format("the handshake must succeed: {}", ec.message()));
            co_await client.AsyncRead(message, ec);
            check(!ec && message.IsText() && message.View() == "hello abc", "handshake headers must reach the server");

            co_await client.AsyncSend(WebSocketMessage{ "echo me", false }, ec);
            co_await client.AsyncRead(message, ec);
            check(!ec && message.IsText() && message.View() == "echo me", "text messages must round-trip");
            std::string const bytes{ "\x00\x01\x02\xff", 4 };
            co_await client.AsyncSend(WebSocketMessage{ bytes, true }, ec);
            co_await client.AsyncRead(message, ec);
            check(!ec && message.IsBinary() && message.Data() == bytes, "binary messages must keep their frame type");

            co_await client.AsyncPing("p", ec);
            check(!ec, "a ping must be sent");
            co_await client.AsyncPing(std::string(200, 'x'), ec);
            check(ec == net::error::message_size && client.IsOpen(), "an oversized ping must be rejected without throwing or closing");

            // Writes from several coroutines are queued, not interleaved.
            int sent{ 0 };
            for (int i{ 0 }; i < 5; ++i) {
                net::co_spawn(ioc, [&client, &sent, i]() -> net::awaitable<void> {
                    boost::system::error_code send_ec;
                    co_await client.AsyncSend(WebSocketMessage{ std::format("m{}", i), false }, send_ec);
                    sent += send_ec ? 0 : 1; }, net::detached);
            }
            std::set<std::string> echoed;
            for (int i{ 0 }; i < 5; ++i) {
                co_await client.AsyncRead(message, ec);
                if (ec) {
                    break;
                }
                echoed.insert(message.Data());
            }
            check(sent == 5 && echoed == std::set<std::string>{ "m0", "m1", "m2", "m3", "m4" }, "concurrent sends must all be delivered intact");

            co_await client.AsyncClose("done", ec);
            check(!client.IsOpen(), "a closed client must report closed");
            co_await client.AsyncSend(WebSocketMessage{ "late", false }, ec);
            check(ec == mhttp::Error::NotInitialized, "sending after close must fail");

            WsClient refused{ ioc, "127.0.0.1", server.Port() };
            co_await refused.AsyncConnect("/reject", mhttp::Headers{}, std::chrono::seconds{ 5 }, ec);
            check(ec && !refused.IsOpen(), "a declined upgrade must fail the connect");

            WsClient tls{ ioc, "127.0.0.1", 1, mhttp::url::Scheme::Tls };
            co_await tls.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 5 }, ec);
            check(ec && !tls.IsOpen(), "an unreachable WSS endpoint must fail without opening a session");
        });
    }

    auto test_websocket_run() -> void {
        WsEchoServer server{ 1 };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Port() };
            std::optional<boost::system::error_code> opened;
            std::vector<std::string> messages;
            bool closed{ false };
            mhttp::Headers headers;
            headers.Set("X-Token", "run");
            client.Run(
                "/ws",
                [&](boost::system::error_code ec) { opened = ec; },
                [&](WebSocketMessage message) -> net::awaitable<void> {
                    messages.push_back(message.Data());
                    boost::system::error_code ec;
                    co_await client.AsyncSend(WebSocketMessage{ messages.size() == 1 ? "again" : "bye", false }, ec);
                },
                [&] { closed = true; },
                headers
            );
            for (int i{ 0 }; i < 300 && !closed; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 10 });
            }
            check(opened.has_value() && !*opened, "the open handler must report a successful connect");
            check(messages == std::vector<std::string>{ "hello run", "again" }, std::format("the message handler must see every message, got {}", messages));
            check(closed && !client.IsOpen(), "the close handler must run once the server closed");
        });
    }

    /**
     * @brief Accept HTTP headers and remain silent until the client closes the socket.
     */
    struct SilentServer {
        net::io_context ioc;
        tcp::acceptor acceptor{
            ioc,
            tcp::endpoint{ net::ip::make_address("127.0.0.1"), 0 }
        };
        std::atomic<int> requests{ 0 };
        std::jthread worker;

        explicit SilentServer(int connections) : worker{ [this, connections] {
                                                     for (int i{ 0 }; i < connections; ++i) {
                                                         boost::system::error_code ec;
                                                         tcp::socket socket{ ioc };
                                                         acceptor.accept(socket, ec);
                                                         if (ec) {
                                                             return;
                                                         }
                                                         boost::beast::flat_buffer buffer;
                                                         http::request_parser<http::empty_body> request;
                                                         http::read_header(socket, buffer, request, ec);
                                                         requests.fetch_add(1);
                                                         std::array<char, 16> data{};
                                                         while (!ec) {
                                                             socket.read_some(net::buffer(data), ec);
                                                         }
                                                     }
                                                 } } {}

        [[nodiscard]] auto Port() const -> std::uint16_t {
            return acceptor.local_endpoint().port();
        }
    };

    /**
     * @brief Both clients must bound and cancel incomplete handshakes and reject overlap.
     */
    auto test_handshake_lifecycle() -> void {
        SilentServer server{ 4 };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            for (int i{ 0 }; i < 2; ++i) {
                ProxyClient client{ ioc, "127.0.0.1", server.Port() };
                client.SetTimeout(i == 0 ? std::chrono::seconds{ 5 } : std::chrono::milliseconds{ 80 });
                std::optional<boost::system::error_code> result;
                net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                    boost::system::error_code ec;
                    co_await client.AsyncConnect("example.com:443", mhttp::Headers{}, ec);
                    result = ec; }, net::detached);
                for (int spin{ 0 }; spin < 500 && server.requests.load() < i + 1; ++spin) {
                    co_await sleep_for(std::chrono::milliseconds{ 1 });
                }
                check(!client.IsOpen(), "an unfinished CONNECT must not report an open tunnel");
                if (i == 0) {
                    boost::system::error_code ec;
                    co_await client.AsyncConnect("example.com:443", mhttp::Headers{}, ec);
                    check(ec == mhttp::Error::OperationInProgress, "overlapping CONNECT calls must be rejected");
                    co_await client.AsyncClose();
                }
                for (int spin{ 0 }; spin < 500 && !result; ++spin) {
                    co_await sleep_for(std::chrono::milliseconds{ 1 });
                }
                check(result && *result == (i == 0 ? make_error_code(net::error::operation_aborted) : make_error_code(boost::beast::error::timeout)), "CONNECT must finish with cancellation or timeout");
                check(!client.IsOpen(), "a cancelled or timed-out CONNECT must remain closed");
            }
            for (int i{ 0 }; i < 2; ++i) {
                WsClient client{ ioc, "127.0.0.1", server.Port() };
                std::optional<boost::system::error_code> result;
                net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                    boost::system::error_code ec;
                    co_await client.AsyncConnect("/ws", mhttp::Headers{}, i == 0 ? std::chrono::seconds{ 5 } : std::chrono::milliseconds{ 80 }, ec);
                    result = ec; }, net::detached);
                for (int spin{ 0 }; spin < 500 && server.requests.load() < i + 3; ++spin) {
                    co_await sleep_for(std::chrono::milliseconds{ 1 });
                }
                check(!client.IsOpen(), "an unfinished upgrade must not report an open WebSocket");
                if (i == 0) {
                    boost::system::error_code ec;
                    co_await client.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 5 }, ec);
                    check(ec == mhttp::Error::OperationInProgress, "overlapping WebSocket handshakes must be rejected");
                    co_await client.AsyncAbort();
                }
                for (int spin{ 0 }; spin < 500 && !result; ++spin) {
                    co_await sleep_for(std::chrono::milliseconds{ 1 });
                }
                check(result && *result == (i == 0 ? make_error_code(net::error::operation_aborted) : make_error_code(boost::beast::error::timeout)), "WebSocket connect must finish with cancellation or timeout");
                check(!client.IsOpen(), "a cancelled or timed-out WebSocket must remain closed");
            }
        });
    }

    /**
     * @brief Reject overlapping reads, abort a pending read, reconnect, and use future APIs.
     */
    auto test_websocket_reuse() -> void {
        WsEchoServer server{ 2 };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Port() };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 5 }, ec);
            WebSocketMessage greeting;
            co_await client.AsyncRead(greeting, ec);
            std::optional<boost::system::error_code> read_result;
            WebSocketMessage pending;
            net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                boost::system::error_code read_ec;
                co_await client.AsyncRead(pending, read_ec);
                read_result = read_ec; }, net::detached);
            co_await sleep_for(std::chrono::milliseconds{ 10 });
            WebSocketMessage overlapping;
            co_await client.AsyncRead(overlapping, ec);
            check(ec == mhttp::Error::OperationInProgress, "a second read must fail without disturbing the first");
            co_await client.AsyncAbort();
            for (int i{ 0 }; i < 200 && !read_result; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 1 });
            }
            check(read_result && *read_result, "aborting must finish a pending read");
            co_await client.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 5 }, ec);
            check(!ec && client.IsOpen(), "the same WebSocket client must reconnect after abort");
            co_await client.AsyncRead(greeting, ec);
            auto sent{ client.Send(WebSocketMessage{ "future message", false }) };
            for (int i{ 0 }; i < 200 && sent.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 1 });
            }
            check(sent.wait_for(std::chrono::seconds{ 0 }) == std::future_status::ready && !sent.get(), "Send must complete its future");
            co_await client.AsyncRead(greeting, ec);
            check(!ec && greeting.View() == "future message", "a future-based send must reach the peer");
            co_await client.AsyncPong("pong", ec);
            check(!ec, "unsolicited pongs must succeed");
            auto closed{ client.Close() };
            for (int i{ 0 }; i < 200 && closed.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 1 });
            }
            check(closed.wait_for(std::chrono::seconds{ 0 }) == std::future_status::ready && !closed.get() && !client.IsOpen(), "Close must finish its future and close the reconnected stream");
        });
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("CONNECT tunnel", test_tunnel);
    run_case("refused tunnel", test_tunnel_refused);
    run_case("websocket messages", test_websocket_messages);
    run_case("websocket run loop", test_websocket_run);
    run_case("handshake lifecycle", test_handshake_lifecycle);
    run_case("websocket reuse", test_websocket_reuse);
    return finish("test_proxy_ws");
}
