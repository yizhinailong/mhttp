/**
 * @file test_websocket.cpp
 * @brief Exercise server WebSockets with WsClient and raw HTTP/frame exchanges.
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
using mhttp::WebSocketConn;
using mhttp::WebSocketMessage;
using mhttp::client::WsClient;

namespace {

    namespace websocket = boost::beast::websocket;

    /**
     * @brief Run a client coroutine with a watchdog so a lost frame cannot hang the suite.
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
        check(finished, "the client coroutine must finish before the watchdog expires");
    }

    /**
     * @brief Yield while another executor advances a session.
     */
    auto sleep_for(std::chrono::milliseconds duration) -> net::awaitable<void> {
        net::steady_timer timer{ co_await net::this_coro::executor, duration };
        co_await timer.async_wait(net::use_awaitable);
    }

    /**
     * @brief Echo one complete message without changing its frame type.
     */
    auto echo(WebSocketConn::WeakPtr weak, WebSocketMessage message) -> net::awaitable<void> {
        if (auto connection{ weak.lock() }) {
            boost::system::error_code ec;
            co_await connection->AsyncSend(message, ec);
        }
    }

    /**
     * @brief Match all path kinds, retain request data, and round-trip messages with WsClient.
     */
    auto test_routes_and_echo() -> void {
        std::atomic<int> opened{ 0 };
        std::atomic<int> closed{ 0 };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            auto& router{ srv.GetRouter() };
            auto const add{ [&](std::string_view path, std::string tag) {
                router.SetWsHandler(path, [&, tag](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                        auto connection{ weak.lock() };
                        ++opened;
                        auto& req{ connection->HttpRequest() };
                        req.Data().Store(std::string{ "session state" });
                        auto const query{ req.Query().Has("q") ? req.Query().At("q") : std::string_view{} };
                        auto const greeting{ std::format("{}|{}|{}|{}|{}", tag, req.PathParam("id"), req.PathParam("*"), query, req["X-Token"]) };
                        boost::system::error_code ec;
                        co_await connection->AsyncSend(WebSocketMessage{ greeting, false }, ec); }, echo, [&](WebSocketConn::WeakPtr weak) {
                        if (auto connection{ weak.lock() }; connection && !connection->IsOpen() && connection->HttpRequest().Data().Fetch<std::string>() == "session state") {
                            ++closed;
                        } });
            } };
            add("/chat/*", "wildcard");
            add("/chat/:id", "parameter");
            add("/chat/{id:\\d+}", "regex");
            add("/chat/me", "static");
            router.SetHttpHandler(Verb::get, "/chat/me", [](mhttp::Request&, mhttp::Response& resp) {
                resp.SetStringContent("ordinary HTTP", "text/plain");
            });
        } };
        Client http_client{ server.Endpoint() };
        auto const ordinary{ http_client.Get("/chat/me") };
        check(ordinary && ordinary->body() == "ordinary HTTP", "HTTP and WebSocket handlers must coexist at the same path");

        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            std::array<std::pair<std::string_view, std::string_view>, 4> const cases{
                std::pair{                    "/chat/me",                  "static||||token" },
                std::pair{                   "/chat/123",                "regex|123|||token" },
                std::pair{ "/chat/a%2Fb?q=hello%20world", "parameter|a/b||hello world|token" },
                std::pair{                   "/chat/a/b",             "wildcard||a/b||token" }
            };
            for (auto const& [target, expected] : cases) {
                WsClient client{ ioc, "127.0.0.1", server.Endpoint().port() };
                mhttp::Headers headers;
                headers.Set("X-Token", "token");
                headers.Set("X-Large", std::string(12000, 'x'));
                boost::system::error_code ec;
                co_await client.AsyncConnect(target, headers, std::chrono::seconds{ 2 }, ec);
                if (!check(!ec, std::format("{} must upgrade: {}", target, ec.message()))) {
                    co_return;
                }
                WebSocketMessage message;
                co_await client.AsyncRead(message, ec);
                check(!ec && message.IsText() && message.View() == expected, "the open callback must see the matched route, parameters, query, and headers");
                co_await client.AsyncSend(WebSocketMessage{ "hello", false }, ec);
                co_await client.AsyncRead(message, ec);
                check(!ec && message.IsText() && message.View() == "hello", "text messages must round-trip");
                std::string binary(70000, '\0');
                binary.back() = static_cast<char>(255);
                co_await client.AsyncSend(WebSocketMessage{ binary, true }, ec);
                co_await client.AsyncRead(message, ec);
                check(!ec && message.IsBinary() && message.View() == binary, "fragmented binary messages must preserve their full payload and type");
                co_await client.AsyncClose(ec);
                check(!ec && !client.IsOpen(), "the peer's close handshake must complete");
            }
        });
        server.server.Stop().get();
        check(opened.load() == 4 && closed.load() == 4, "every established session must call open and close exactly once");
    }

    /**
     * @brief Build a client-masked small frame for the prefetch regression test.
     */
    auto masked_frame(unsigned char opcode, std::string_view payload) -> std::string {
        std::string frame;
        frame += static_cast<char>(0x80 | opcode);
        frame += static_cast<char>(0x80 | payload.size());
        std::string_view const mask{ "abcd" };
        frame += mask;
        for (std::size_t i{ 0 }; i < payload.size(); ++i) {
            frame += static_cast<char>(payload[i] ^ mask[i % mask.size()]);
        }
        return frame;
    }

    /**
     * @brief Preserve a WebSocket frame delivered in the same TCP write as the HTTP header.
     */
    auto test_prefetched_frame() -> void {
        std::atomic<int> closed{ 0 };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            srv.GetRouter().SetWsHandler("/ws", [](WebSocketConn::WeakPtr) {}, echo, [&](WebSocketConn::WeakPtr) { ++closed; });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            tcp::socket socket{ ioc };
            boost::system::error_code ec;
            co_await socket.async_connect(server.Endpoint(), net::redirect_error(net::use_awaitable, ec));
            std::string const request{ "GET /ws HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" };
            auto const sent{ request + masked_frame(1, "prefetched") };
            co_await net::async_write(socket, net::buffer(sent), net::redirect_error(net::use_awaitable, ec));
            boost::beast::flat_buffer buffer;
            http::response_parser<http::empty_body> parser;
            co_await http::async_read_header(socket, buffer, parser, net::redirect_error(net::use_awaitable, ec));
            if (!check(!ec && parser.get().result() == Status::switching_protocols, "a prefetched frame must not prevent the upgrade")) {
                co_return;
            }
            while (buffer.size() < 12 && !ec) {
                auto const size{ co_await socket.async_read_some(buffer.prepare(64), net::redirect_error(net::use_awaitable, ec)) };
                buffer.commit(size);
            }
            auto const reply{ boost::beast::buffers_to_string(buffer.data()) };
            check(!ec && reply.size() >= 12 && static_cast<unsigned char>(reply[0]) == 0x81 && reply[1] == 10 && reply.substr(2, 10) == "prefetched", "the first buffered frame must be delivered and echoed intact");
            socket.close(ec);
        });
        server.server.Stop().get();
        check(closed.load() == 1, "abrupt peer shutdown must still call close once");
    }

    /**
     * @brief Unknown upgrades use HTTP routing; invalid handshakes never invoke WS callbacks.
     */
    auto test_http_fallback_and_rejection() -> void {
        std::atomic<int> opened{ 0 };
        std::atomic<int> closed{ 0 };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            srv.GetRouter().SetWsHandler("/ws", [&](WebSocketConn::WeakPtr) { ++opened; }, echo, [&](WebSocketConn::WeakPtr) { ++closed; });
            srv.GetRouter().SetHttpHandler(Verb::get, "/plain", [](mhttp::Request&, mhttp::Response& resp) { resp.SetStringContent("HTTP fallback", "text/plain"); });
        } };
        auto const upgrade{ [](std::string_view target) {
            return make_request(Verb::get, target, {
            },
                                { { "Connection", "Upgrade" }, { "Upgrade", "websocket" }, { "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==" }, { "Sec-WebSocket-Version", "13" } });
        } };
        {
            Client client{ server.Endpoint() };
            auto const response{ client.Send(upgrade("/plain")) };
            check(response && response->result() == Status::ok && response->body() == "HTTP fallback", "unmatched upgrades must use the ordinary HTTP route");
        }
        {
            Client client{ server.Endpoint() };
            auto const response{ client.Send(upgrade("/missing")) };
            check(response && response->result() == Status::not_found, "unknown upgrade paths must receive 404");
        }
        {
            Client client{ server.Endpoint() };
            auto const response{ client.Get("/ws") };
            check(response && response->result() == Status::not_found, "a WebSocket-only route must not claim an ordinary GET");
        }
        for (int scenario{ 0 }; scenario < 4; ++scenario) {
            Client client{ server.Endpoint() };
            auto request{ upgrade("/ws") };
            if (scenario == 0) {
                request.erase(Field::sec_websocket_key);
            } else if (scenario == 1) {
                request.set(Field::sec_websocket_version, "12");
            } else if (scenario == 2) {
                request.method(Verb::post);
            } else {
                request.body() = "body";
                request.prepare_payload();
            }
            auto const response{ client.Send(request) };
            if (scenario == 2) {
                check(response && response->result() == Status::not_found, "POST is not an upgrade and must use ordinary HTTP routing");
            } else {
                check(response && (response->result() == Status::bad_request || response->result() == Status::upgrade_required), "invalid upgrades must receive an explicit HTTP rejection");
            }
        }
        server.server.Stop().get();
        check(opened.load() == 0 && closed.load() == 0, "failed upgrades must not invoke established-session callbacks");
    }

    /**
     * @brief Queue messages across executors, exchange control frames, and close from the server.
     */
    auto test_server_operations() -> void {
        std::promise<std::shared_ptr<WebSocketConn>> connected;
        auto ready{ connected.get_future() };
        std::atomic<int> closed{ 0 };
        std::atomic<bool> controls_ok{ false };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            srv.GetRouter().SetWsHandler("/ws", [&](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                    auto connection{ weak.lock() };
                    connected.set_value(connection);
                    boost::system::error_code ec;
                    co_await connection->AsyncPing(std::string(126, 'x'), ec);
                    bool const rejected_ping{ ec == net::error::message_size && connection->IsOpen() };
                    co_await connection->AsyncClose(std::string(124, 'x'), ec);
                    bool const rejected_close{ ec == net::error::message_size && connection->IsOpen() };
                    co_await connection->AsyncPing("ping", ec);
                    bool const ping_ok{ !ec };
                    co_await connection->AsyncPong("pong", ec);
                    controls_ok.store(rejected_ping && rejected_close && ping_ok && !ec);
                    co_await connection->AsyncSend(WebSocketMessage{ "ready", false }, ec); }, [](WebSocketConn::WeakPtr weak, WebSocketMessage message) -> net::awaitable<void> {
                    if (auto connection{ weak.lock() }; connection && message.View() == "bye") {
                        boost::system::error_code ec;
                        co_await connection->AsyncClose("done", ec);
                    } }, [&](WebSocketConn::WeakPtr) { ++closed; });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Endpoint().port() };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 2 }, ec);
            WebSocketMessage message;
            co_await client.AsyncRead(message, ec);
            if (!check(!ec && message.View() == "ready" && controls_ok.load(), "control frames must be handled while awaiting the first message")) {
                co_return;
            }
            auto connection{ ready.get() };
            std::vector<std::future<boost::system::error_code>> sent;
            for (int i{ 0 }; i < 8; ++i) {
                sent.push_back(connection->Send(WebSocketMessage{ std::format("message {}", i), false }));
            }
            std::set<std::string> received;
            for (int i{ 0 }; i < 8; ++i) {
                co_await client.AsyncRead(message, ec);
                if (ec) {
                    break;
                }
                received.insert(message.Data());
            }
            bool sends_ok{ true };
            for (auto& result : sent) {
                for (int i{ 0 }; i < 200 && result.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready; ++i) {
                    co_await sleep_for(std::chrono::milliseconds{ 1 });
                }
                sends_ok = sends_ok && result.wait_for(std::chrono::seconds{ 0 }) == std::future_status::ready && !result.get();
            }
            check(received.size() == 8 && sends_ok, "cross-executor sends must all complete without interleaving frames");
            co_await client.AsyncSend(WebSocketMessage{ "bye", false }, ec);
            co_await client.AsyncRead(message, ec);
            check(ec == websocket::error::closed && !client.IsOpen(), "the server must complete its normal close handshake");
        });
        server.server.Stop().get();
        check(closed.load() == 1, "server-initiated close must call close once");
    }

    /**
     * @brief Stop must cancel an idle read and blocked/queued writes before unregistering a session.
     */
    auto test_shutdown_and_retained_handle() -> void {
        std::promise<std::shared_ptr<WebSocketConn>> connected;
        auto ready{ connected.get_future() };
        std::atomic<int> closed{ 0 };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            srv.GetRouter().SetWsHandler("/ws", [&](WebSocketConn::WeakPtr weak) { connected.set_value(weak.lock()); }, echo, [&](WebSocketConn::WeakPtr) { ++closed; });
        } };
        std::shared_ptr<WebSocketConn> retained;
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Endpoint().port() };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 2 }, ec);
            if (!check(!ec, "the session must upgrade before shutdown")) {
                co_return;
            }
            while (ready.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready) {
                co_await sleep_for(std::chrono::milliseconds{ 1 });
            }
            retained = ready.get();
            std::vector<std::future<boost::system::error_code>> sends;
            for (int i{ 0 }; i < 3; ++i) {
                sends.push_back(retained->Send(WebSocketMessage{ std::string(4 * 1024 * 1024, 'x'), true }));
            }
            co_await sleep_for(std::chrono::milliseconds{ 10 });
            auto stopped{ server.server.Stop() };
            for (int i{ 0 }; i < 1000 && stopped.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready; ++i) {
                co_await sleep_for(std::chrono::milliseconds{ 1 });
            }
            check(stopped.wait_for(std::chrono::seconds{ 0 }) == std::future_status::ready && closed.load() == 1 && !retained->IsOpen(), "Stop must drain the WebSocket and run its close callback");
            for (auto& result : sends) {
                for (int i{ 0 }; i < 200 && result.wait_for(std::chrono::seconds{ 0 }) != std::future_status::ready; ++i) {
                    co_await sleep_for(std::chrono::milliseconds{ 1 });
                }
                if (check(result.wait_for(std::chrono::seconds{ 0 }) == std::future_status::ready, "queued sends must finish during shutdown")) {
                    static_cast<void>(result.get());
                }
            }
            co_await retained->AsyncSend(WebSocketMessage{ "late", false }, ec);
            check(ec == mhttp::Error::NotInitialized, "retained handles must reject sends after shutdown");
            co_await client.AsyncAbort();
        });
        WebSocketConn::WeakPtr weak{ retained };
        retained.reset();
        check(weak.expired(), "the connection must not retain an ownership cycle after shutdown");
    }

    /**
     * @brief A message larger than the configured body limit must end the WebSocket session.
     */
    auto test_message_limit() -> void {
        std::atomic<int> messages{ 0 };
        std::atomic<int> closed{ 0 };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            srv.SetBodyLimit(8);
            srv.GetRouter().SetWsHandler("/ws", [](WebSocketConn::WeakPtr) {}, [&](WebSocketConn::WeakPtr, WebSocketMessage) { ++messages; }, [&](WebSocketConn::WeakPtr) { ++closed; });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Endpoint().port() };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 2 }, ec);
            check(!ec, "small message limits must not reject a body-less upgrade");
            co_await client.AsyncSend(WebSocketMessage{ "too large", false }, ec);
            WebSocketMessage message;
            co_await client.AsyncRead(message, ec);
            check(ec && !client.IsOpen(), "oversized messages must close the connection");
        });
        server.server.Stop().get();
        check(messages.load() == 0 && closed.load() == 1, "an oversized message must not reach the callback");
    }

    /**
     * @brief A zero limit accepts empty messages and rejects a one-byte message.
     */
    auto test_zero_message_limit() -> void {
        std::atomic<int> messages{ 0 };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            srv.SetBodyLimit(0);
            srv.GetRouter().SetWsHandler("/ws", [](WebSocketConn::WeakPtr) {}, [&](WebSocketConn::WeakPtr, WebSocketMessage) { ++messages; }, [](WebSocketConn::WeakPtr) {});
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient client{ ioc, "127.0.0.1", server.Endpoint().port() };
            boost::system::error_code ec;
            co_await client.AsyncConnect("/ws", mhttp::Headers{}, std::chrono::seconds{ 2 }, ec);
            if (!check(!ec, "a zero body limit must still permit an empty upgrade")) {
                co_return;
            }
            co_await client.AsyncSend(WebSocketMessage{ "", false }, ec);
            co_await client.AsyncSend(WebSocketMessage{ "x", false }, ec);
            WebSocketMessage message;
            co_await client.AsyncRead(message, ec);
            check(ec && !client.IsOpen(), "a zero limit must reject a one-byte message");
        });
        server.server.Stop().get();
        check(messages.load() == 1, "only the empty message may reach the callback at a zero limit");
    }

    /**
     * @brief Callback failures close the session without preventing cleanup or later requests.
     */
    auto test_callback_failures() -> void {
        std::atomic<int> closed{ 0 };
        TestServer server{ [&](mhttp::HttpServer& srv) {
            auto& router{ srv.GetRouter() };
            auto const close{ [&](WebSocketConn::WeakPtr) { ++closed; } };
            router.SetWsHandler("/open", [](WebSocketConn::WeakPtr) { throw std::runtime_error{ "open failure" }; }, echo, close);
            router.SetWsHandler("/message", [](WebSocketConn::WeakPtr) {}, [](WebSocketConn::WeakPtr, WebSocketMessage) { throw std::runtime_error{ "message failure" }; }, close);
            router.SetWsHandler("/close", [](WebSocketConn::WeakPtr) {}, echo, [&](WebSocketConn::WeakPtr) {
                ++closed;
                throw std::runtime_error{ "close failure" }; });
            router.SetHttpHandler(Verb::get, "/health", [](mhttp::Request&, mhttp::Response& response) { response.SetStringContent("ok", "text/plain"); });
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            for (std::string_view const path : { "/open", "/message", "/close" }) {
                WsClient client{ ioc, "127.0.0.1", server.Endpoint().port() };
                boost::system::error_code ec;
                co_await client.AsyncConnect(path, mhttp::Headers{}, std::chrono::seconds{ 2 }, ec);
                if (!check(!ec, "the failure callback must run after a successful handshake")) {
                    continue;
                }
                if (path == "/close") {
                    co_await client.AsyncClose(ec);
                    check(!ec, "a close callback exception must not break the completed close handshake");
                } else {
                    if (path == "/message") {
                        co_await client.AsyncSend(WebSocketMessage{ "trigger", false }, ec);
                    }
                    WebSocketMessage message;
                    co_await client.AsyncRead(message, ec);
                    check(ec && !client.IsOpen(), "an open or message callback failure must abort its session");
                }
            }
        });
        Client client{ server.Endpoint() };
        auto const response{ client.Get("/health") };
        check(response && response->body() == "ok", "callback failures must leave the server usable");
        server.server.Stop().get();
        check(closed.load() == 3, "all failed callback paths must finish their close callback once");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("routes and echo", test_routes_and_echo);
    run_case("prefetched frame", test_prefetched_frame);
    run_case("HTTP fallback and rejection", test_http_fallback_and_rejection);
    run_case("server operations", test_server_operations);
    run_case("shutdown and retained handle", test_shutdown_and_retained_handle);
    run_case("message limit", test_message_limit);
    run_case("zero message limit", test_zero_message_limit);
    run_case("callback failures", test_callback_failures);
    return finish("test_websocket");
}
