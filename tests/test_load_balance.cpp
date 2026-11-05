/**
 * @file test_load_balance.cpp
 * @brief Exercise shared proxy balancing through real HTTP and WebSocket backends.
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
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

import std;
import mhttp;

#include "support.hpp"

using namespace test_support;
using mhttp::Field;
using mhttp::Status;
using mhttp::UpstreamBackend;
using mhttp::UpstreamStrategy;
using mhttp::Verb;
using mhttp::WebSocketConn;
using mhttp::WebSocketMessage;
using mhttp::client::BodyMode;
using mhttp::client::HttpClient;
using mhttp::client::WsClient;

namespace {

    /**
     * @brief Yield without blocking any of the real servers' execution contexts.
     */
    auto delay(std::chrono::milliseconds duration) -> net::awaitable<void> {
        net::steady_timer timer{ co_await net::this_coro::executor, duration };
        co_await timer.async_wait(net::use_awaitable);
    }

    /**
     * @brief Wait a bounded interval for cross-executor work to reach a gate.
     */
    auto wait_for(std::atomic<int> const& counter, int value) -> net::awaitable<bool> {
        for (int i{ 0 }; i < 200 && counter.load() < value; ++i) {
            co_await delay(std::chrono::milliseconds{ 5 });
        }
        co_return counter.load() >= value;
    }

    /**
     * @brief Bound coroutine tests so a leaked reservation cannot hang the test program.
     */
    auto run_client(std::function<net::awaitable<void>(net::io_context&)> const& body) -> void {
        net::io_context ioc;
        net::steady_timer watchdog{ ioc, std::chrono::seconds{ 10 } };
        bool finished{ false };
        watchdog.async_wait([&](boost::system::error_code ec) { if (!ec) { ioc.stop(); } });
        net::co_spawn(ioc, [&]() -> net::awaitable<void> {
            co_await body(ioc);
            finished = true;
            watchdog.cancel(); }, net::detached);
        ioc.run();
        check(finished, "the balanced exchange must finish before its watchdog expires");
    }

    /**
     * @brief Build a local origin in the required proxy scheme.
     */
    auto origin(TestServer const& server, bool websocket = false) -> std::string {
        return std::format("{}://127.0.0.1:{}", websocket ? "ws" : "http", server.Endpoint().port());
    }

    /**
     * @brief Register distinguishable HTTP and WebSocket responses on one upstream.
     */
    auto configure_backend(mhttp::HttpServer& server, std::string tag) -> void {
        server.GetRouter().SetHttpHandler(Verb::get, "/*", [tag](mhttp::Request& request, mhttp::Response& response) {
            response.SetStringContent(tag + "|" + std::string{ request.Target() }, "text/plain");
        });
        server.GetRouter().SetWsHandler("/*", [tag](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
            auto connection{ weak.lock() };
            boost::system::error_code ec;
            co_await connection->AsyncSend(WebSocketMessage{ tag + "|" + std::string{ connection->HttpRequest().Target() }, false }, ec); }, [](WebSocketConn::WeakPtr, WebSocketMessage) {}, [](WebSocketConn::WeakPtr) {});
    }

    /**
     * @brief Open a client and read its backend's greeting.
     */
    auto ws_tag(WsClient& client, std::string_view path = "/ws") -> net::awaitable<std::string> {
        boost::system::error_code ec;
        co_await client.AsyncConnect(path, {}, std::chrono::seconds{ 2 }, ec);
        if (!check(!ec, "a healthy selected backend must accept the WebSocket")) {
            co_return std::string{};
        }
        WebSocketMessage message;
        co_await client.AsyncRead(message, ec);
        check(!ec, "the upstream greeting must reach the downstream");
        co_return std::string{ message.View() };
    }

    /**
     * @brief Controllable upstream that can fail before headers, hold requests, or truncate bodies.
     */
    class ControlledUpstream {
    private:
        net::io_context m_ioc; ///< Raw upstream context.
        tcp::acceptor m_acceptor{
            m_ioc,
            tcp::endpoint{ net::ip::address_v4::loopback(), 0 }
        };                     ///< Listener.
        std::jthread m_worker; ///< Joined explicitly before the public flags are destroyed.

    public:
        std::atomic<bool> failing{ false }; ///< Close after the request instead of replying.
        std::atomic<bool> hold{ false };    ///< Gate paths beginning with /hold.
        std::atomic<int> requests{ 0 };     ///< Received complete request headers.
        std::atomic<int> held{ 0 };         ///< Requests that reached the gate.
        std::atomic<int> cancelled{ 0 };    ///< Peers closed while waiting at the gate.

        /**
         * @brief Serve independent connections so one pending probe cannot block other traffic.
         */
        ControlledUpstream() {
            net::co_spawn(m_ioc, [this]() -> net::awaitable<void> {
                for (;;) {
                    boost::system::error_code ec;
                    auto socket{ co_await m_acceptor.async_accept(net::redirect_error(net::use_awaitable, ec)) };
                    if (ec) { co_return; }
                    net::co_spawn(m_ioc, [this, socket = std::move(socket)]() mutable -> net::awaitable<void> {
                        co_await serve(std::move(socket));
                    }, net::detached);
                } }, net::detached);
            m_worker = std::jthread{ [this] { m_ioc.run(); } };
        }

        /**
         * @brief Stop pending accept and gate waits without depending on peer cooperation.
         */
        ~ControlledUpstream() {
            m_ioc.stop();
            m_worker.join();
        }

        /**
         * @brief Return the local URL for either forwarding API.
         */
        auto Url(bool websocket = false) const -> std::string {
            return std::format("{}://127.0.0.1:{}", websocket ? "ws" : "http", m_acceptor.local_endpoint().port());
        }

    private:
        /**
         * @brief Serve one request with bounded gates and a non-consuming disconnect check.
         */
        auto serve(tcp::socket socket) -> net::awaitable<void> {
            boost::system::error_code ec;
            boost::beast::flat_buffer buffer;
            http::request<http::string_body> request;
            co_await http::async_read(socket, buffer, request, net::redirect_error(net::use_awaitable, ec));
            if (ec) {
                co_return;
            }
            ++requests;
            auto const target{
                std::string_view{ request.target().data(), request.target().size() }
            };
            if (hold.load() && target.starts_with("/hold")) {
                ++held;
                socket.non_blocking(true, ec);
                auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 3 } };
                while (hold.load() && std::chrono::steady_clock::now() < deadline) {
                    std::array<char, 1> probe{};
                    static_cast<void>(socket.receive(net::buffer(probe), tcp::socket::message_peek, ec));
                    if (ec && ec != net::error::would_block && ec != net::error::try_again) {
                        ++cancelled;
                        co_return;
                    }
                    ec = {};
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
            }
            if (failing.load()) {
                co_return;
            }
            if (boost::beast::websocket::is_upgrade(request)) {
                boost::beast::websocket::stream<tcp::socket> stream{ std::move(socket) };
                co_await stream.async_accept(request, net::redirect_error(net::use_awaitable, ec));
                if (!ec) {
                    co_await stream.async_write(net::buffer(std::string_view{ "A" }), net::redirect_error(net::use_awaitable, ec));
                }
                if (!ec) {
                    co_await stream.async_read(buffer, net::redirect_error(net::use_awaitable, ec));
                }
                co_return;
            }
            http::response<http::string_body> response{ Status::ok, 11 };
            response.keep_alive(false);
            response.body() = target == "/truncated" ? "short" : "A";
            response.prepare_payload();
            if (target == "/truncated") {
                response.content_length(100);
            }
            co_await http::async_write(socket, response, net::redirect_error(net::use_awaitable, ec));
        }
    };

    /**
     * @brief Check even and weighted HTTP distribution with per-backend encoded path rewriting.
     */
    auto test_http_distribution() -> void {
        TestServer a{ [](mhttp::HttpServer& server) { configure_backend(server, "A"); } };
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        for (auto const strategy : { UpstreamStrategy::RoundRobin, UpstreamStrategy::WeightedRoundRobin }) {
            TestServer proxy{ [&](mhttp::HttpServer& server) {
                mhttp::ReverseProxyConfig config;
                config.balancing.strategy = strategy;
                server.SetReverseProxy("/api", std::vector<UpstreamBackend>{
                                                   { origin(a) + "/alpha", 3 },
                                                   {  origin(b) + "/beta", 1 }
                },
                                       config);
            } };
            Client client{ proxy.Endpoint() };
            int a_count{ 0 };
            for (int i{ 0 }; i < 16; ++i) {
                auto const response{ client.Get("/api/a%2Fb?q=%2F") };
                if (!check(response.has_value(), "a balanced HTTP request must complete")) {
                    continue;
                }
                bool const from_a{ response->body().starts_with("A|") };
                a_count += from_a;
                check(response->body() == (from_a ? "A|/alpha/a%2Fb?q=%2F" : "B|/beta/a%2Fb?q=%2F"), "the selected backend's base path must be used without decoding the target");
                if (strategy == UpstreamStrategy::RoundRobin) {
                    check(from_a == (i % 2 == 0), "round robin must rotate on a persistent downstream connection");
                }
            }
            check(a_count == (strategy == UpstreamStrategy::RoundRobin ? 8 : 12), "HTTP selection must honor the configured distribution");
        }
    }

    /**
     * @brief Use the same policies for WebSocket handshakes and release each closed session.
     */
    auto test_ws_distribution() -> void {
        TestServer a{ [](mhttp::HttpServer& server) { configure_backend(server, "A"); } };
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        for (auto const strategy : { UpstreamStrategy::RoundRobin, UpstreamStrategy::WeightedRoundRobin }) {
            TestServer proxy{ [&](mhttp::HttpServer& server) {
                mhttp::WsForwardConfig config;
                config.balancing.strategy = strategy;
                server.SetWsForward("/ws", std::vector<UpstreamBackend>{
                                               { origin(a, true) + "/alpha", 3 },
                                               { origin(b,  true) + "/beta", 1 }
                },
                                    config);
            } };
            run_client([&](net::io_context& ioc) -> net::awaitable<void> {
                int a_count{ 0 };
                for (int i{ 0 }; i < 8; ++i) {
                    WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
                    auto const tag{ co_await ws_tag(client, "/ws/room?q=1") };
                    bool const from_a{ tag.starts_with("A|") };
                    a_count += from_a;
                    check(tag == (from_a ? "A|/alpha/room?q=1" : "B|/beta/room?q=1"), "each WebSocket must use the selected backend's path");
                    if (strategy == UpstreamStrategy::RoundRobin) {
                        check(from_a == (i % 2 == 0), "WebSocket round robin must rotate by session");
                    }
                    boost::system::error_code ec;
                    co_await client.AsyncClose(ec);
                }
                check(a_count == (strategy == UpstreamStrategy::RoundRobin ? 4 : 6), "WebSocket selection must honor configured weights");
            });
        }
    }

    /**
     * @brief Keep a streaming HTTP response counted until its body finishes, then release it.
     */
    auto test_http_least_and_capacity() -> void {
        std::atomic<bool> release{ false };
        TestServer a{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/*", [&](mhttp::Request& request, mhttp::Response& response) -> net::awaitable<void> {
                if (request.Path() != "/hold") {
                    response.SetStringContent("A", "text/plain");
                    co_return;
                }
                auto writer{ response.CreateStreamWriter() };
                boost::system::error_code ec;
                co_await writer->WriteHeader(Status::ok, {}, mhttp::StreamWriter::Mode::Chunked, ec);
                co_await writer->WriteBody(net::buffer(std::string_view{ "A" }), true, ec);
                auto const deadline{ std::chrono::steady_clock::now() + std::chrono::seconds{ 3 } };
                while (!release.load() && std::chrono::steady_clock::now() < deadline) {
                    co_await delay(std::chrono::milliseconds{ 5 });
                }
                co_await writer->WriteBody({}, false, ec);
            });
        } };
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            config.balancing.strategy = UpstreamStrategy::LeastConnections;
            server.SetReverseProxy("/api", std::vector<UpstreamBackend>{ { origin(a) }, { origin(b) } }, config);
            config.max_connections = 1;
            config.acquire_timeout = std::chrono::milliseconds{ 50 };
            server.SetReverseProxy("/capacity", std::vector<UpstreamBackend>{ { origin(a) } }, config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient held{ ioc, origin(proxy) };
            HttpClient other{ ioc, origin(proxy) };
            auto stream{ co_await held.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/api/hold" }, BodyMode::Lazy) };
            if (check(stream.has_value(), "streaming response headers must arrive")) {
                for (int i{ 0 }; i < 3; ++i) {
                    auto response{ co_await other.AsyncGet("/api/tag") };
                    check(response && response->AsString() == "B|/tag", "least connections must keep avoiding a response with an unfinished body");
                }
                release.store(true);
                check(!(co_await stream->Drain()), "the held body must drain");
                bool selected_a{ false };
                for (int i{ 0 }; i < 100 && !selected_a; ++i) {
                    auto response{ co_await other.AsyncGet("/api/tag") };
                    selected_a = response && response->AsString() == "A";
                    if (!selected_a) {
                        co_await delay(std::chrono::milliseconds{ 5 });
                    }
                }
                check(selected_a, "the backend reservation must be released after the complete body");
            }
            release.store(false);
            auto capacity{ co_await held.AsyncSendRequest(mhttp::client::Request{ Verb::get, "/capacity/hold" }, BodyMode::Lazy) };
            auto timeout{ co_await other.AsyncGet("/capacity/tag") };
            check(timeout && timeout->Result() == Status::gateway_timeout, "a saturated pool must keep its 504 behavior");
            release.store(true);
            if (capacity) {
                static_cast<void>(co_await capacity->Drain());
            }
            auto healthy{ co_await other.AsyncGet("/capacity/tag") };
            check(healthy && healthy->AsString() == "A", "pool capacity timeouts must not cool a healthy backend");
        });
        release.store(true);
    }

    /**
     * @brief Count active WebSockets, release graceful and abrupt closes, and drain both origins.
     */
    auto test_ws_least_and_stop() -> void {
        std::atomic<int> closed{ 0 };
        auto const setup{ [&](std::string tag) {
            return [&, tag](mhttp::HttpServer& server) {
                server.GetRouter().SetWsHandler("/*", [tag](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                    boost::system::error_code ec;
                    co_await weak.lock()->AsyncSend(WebSocketMessage{ tag, false }, ec); }, [](WebSocketConn::WeakPtr, WebSocketMessage) {}, [&](WebSocketConn::WeakPtr) { ++closed; });
            };
        } };
        TestServer a{ setup("A") };
        TestServer b{ setup("B") };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::WsForwardConfig config;
            config.balancing.strategy = UpstreamStrategy::LeastConnections;
            server.SetWsForward("/ws", std::vector<UpstreamBackend>{ { origin(a, true) }, { origin(b, true) } }, config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient first{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            WsClient second{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            WsClient third{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            WsClient fourth{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            WsClient fifth{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            check((co_await ws_tag(first)) == "A" && (co_await ws_tag(second)) == "B" && (co_await ws_tag(third)) == "A", "active sessions and rotating ties must guide selection");
            boost::system::error_code ec;
            co_await second.AsyncClose(ec);
            check(co_await wait_for(closed, 1), "a graceful close must reach the upstream");
            co_await delay(std::chrono::milliseconds{ 10 });
            check((co_await ws_tag(fourth)) == "B", "a closed session must release its backend load");
            co_await first.AsyncAbort();
            check(co_await wait_for(closed, 2), "an abrupt downstream close must release the upstream");
            co_await delay(std::chrono::milliseconds{ 10 });
            check((co_await ws_tag(fifth)) == "A", "aborted sessions must release their backend load");
            co_await proxy.server.AsyncStop();
            check(co_await wait_for(closed, 5), "stop must drain active sessions on every backend");
        });
    }

    /**
     * @brief Count a pending WebSocket handshake before it becomes an established session.
     */
    auto test_pending_handshake_load() -> void {
        ControlledUpstream a;
        a.hold.store(true);
        std::atomic<int> closed{ 0 };
        TestServer b{ [&](mhttp::HttpServer& server) {
            server.GetRouter().SetWsHandler("/*", [](WebSocketConn::WeakPtr weak) -> net::awaitable<void> {
                boost::system::error_code ec;
                co_await weak.lock()->AsyncSend(WebSocketMessage{ "B", false }, ec); }, [](WebSocketConn::WeakPtr, WebSocketMessage) {}, [&](WebSocketConn::WeakPtr) { ++closed; });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::WsForwardConfig config;
            config.balancing.strategy = UpstreamStrategy::LeastConnections;
            server.SetWsForward("/ws", std::vector<UpstreamBackend>{ { a.Url(true) }, { origin(b, true) } }, config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient pending{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            bool done{ false };
            boost::system::error_code pending_error;
            net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                co_await pending.AsyncConnect("/ws/hold", {}, std::chrono::seconds{ 2 }, pending_error);
                done = true; }, net::detached);
            check(co_await wait_for(a.held, 1), "the first upstream handshake must reach its gate");
            for (int i{ 0 }; i < 2; ++i) {
                WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
                check((co_await ws_tag(client)) == "B", "pending handshakes must count toward least-connections load");
                boost::system::error_code ec;
                co_await client.AsyncClose(ec);
                check(co_await wait_for(closed, i + 1), "the selected upstream's close callback must finish");
                // The downstream close frame can complete before the relay releases its lease.
                co_await delay(std::chrono::milliseconds{ 20 });
            }
            co_await pending.AsyncAbort();
            while (!done) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            check(static_cast<bool>(pending_error), "the held handshake must be cancellable");
            check(co_await wait_for(a.cancelled, 1), "cancelling a handshake must close its upstream");
            a.hold.store(false);
            co_await delay(std::chrono::milliseconds{ 10 });
            WsClient next{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            check((co_await ws_tag(next)) == "A", "a cancelled handshake must release load without cooling the healthy backend");
            boost::system::error_code ec;
            co_await next.AsyncClose(ec);
        });
    }

    /**
     * @brief Skip failed nodes, recover after cooldown, and fail fast when all nodes are cooling.
     */
    auto test_failure_and_recovery() -> void {
        ControlledUpstream a;
        a.failing.store(true);
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            config.balancing.failure_cooldown = std::chrono::milliseconds{ 100 };
            server.SetReverseProxy("/api", std::vector<UpstreamBackend>{ { a.Url() }, { origin(b) } }, config);
            config.balancing.failure_cooldown = std::chrono::steady_clock::duration::max();
            server.SetReverseProxy("/alone", std::vector<UpstreamBackend>{ { a.Url() } }, config);
            config.balancing.failure_cooldown = std::chrono::steady_clock::duration::zero();
            server.SetReverseProxy("/zero", std::vector<UpstreamBackend>{ { a.Url() } }, config);
        } };
        auto const get{ [&](std::string_view path) {
            Client client{ proxy.Endpoint() };
            return client.Get(path);
        } };
        auto const failed{ get("/api/tag") };
        check(failed && failed->result() == Status::bad_gateway, "a selected backend failure must retain the 502 response");
        for (int i{ 0 }; i < 3; ++i) {
            auto const response{ get("/api/tag") };
            check(response && response->body().starts_with("B|"), "new requests must skip the cooling backend");
        }
        check(a.requests.load() == 1, "cooldown must avoid reconnecting for every request");
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            a.failing.store(false);
            co_await delay(std::chrono::milliseconds{ 130 });
            HttpClient http{ ioc, origin(proxy) };
            auto recovered{ co_await http.AsyncGet("/api/tag") };
            check(recovered && recovered->AsString() == "A", "a due backend must receive a passive recovery probe even with another healthy backend");
        });
        a.failing.store(true);
        auto const alone{ get("/alone/tag") };
        auto const unavailable{ get("/alone/tag") };
        check(alone && alone->result() == Status::bad_gateway && unavailable && unavailable->result() == Status::service_unavailable, "all unavailable backends must return 503 after the initial failure");
        for (int i{ 0 }; i < 2; ++i) {
            auto const response{ get("/zero/tag") };
            check(response && response->result() == Status::bad_gateway, "a zero cooldown must allow immediate fresh attempts");
        }
    }

    /**
     * @brief Reserve one recovery probe and make cancelled probes immediately eligible again.
     */
    auto test_probe_and_cancellation() -> void {
        ControlledUpstream a;
        a.failing.store(true);
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            config.balancing.failure_cooldown = std::chrono::milliseconds{ 200 };
            server.SetReverseProxy("/api", std::vector<UpstreamBackend>{ { a.Url() }, { origin(b) } }, config);
        } };
        Client initial{ proxy.Endpoint() };
        auto const failure{ initial.Get("/api/tag") };
        check(failure && failure->result() == Status::bad_gateway, "the first failure must arm cooldown");
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            a.failing.store(false);
            a.hold.store(true);
            co_await delay(std::chrono::milliseconds{ 230 });
            HttpClient probe{ ioc, origin(proxy) };
            HttpClient other{ ioc, origin(proxy) };
            bool done{ false };
            net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                static_cast<void>(co_await probe.AsyncGet("/api/hold"));
                done = true; }, net::detached);
            check(co_await wait_for(a.held, 1), "one due recovery probe must enter the gate");
            for (int i{ 0 }; i < 2; ++i) {
                auto response{ co_await other.AsyncGet("/api/tag") };
                check(response && response->AsString().starts_with("B|"), "other requests must avoid the half-open backend");
            }
            check(a.requests.load() == 2, "there must be at most one recovery probe for a backend");
            co_await probe.AsyncClose();
            while (!done) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            check(co_await wait_for(a.cancelled, 1), "downstream cancellation must close the held upstream");
            a.hold.store(false);
            co_await delay(std::chrono::milliseconds{ 10 });
            auto recovered{ co_await other.AsyncGet("/api/tag") };
            check(recovered && recovered->AsString() == "A", "a cancelled probe must release its reservation without a new failure cooldown");
        });
    }

    /**
     * @brief Ignore old successes, classify truncated bodies as faults, and preserve HTTP errors.
     */
    auto test_failure_attribution() -> void {
        ControlledUpstream a;
        a.hold.store(true);
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        TestServer application{ [](mhttp::HttpServer& server) {
            server.GetRouter().SetHttpHandler(Verb::get, "/*", [](mhttp::Request&, mhttp::Response& response) {
                response.SetStringContent("application", "text/plain", Status::service_unavailable);
            });
        } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            server.SetReverseProxy("/api", std::vector<UpstreamBackend>{ { a.Url() }, { origin(b) } }, config);
            server.SetReverseProxy("/body", std::vector<UpstreamBackend>{ { a.Url() }, { origin(b) } }, config);
            server.SetReverseProxy("/application", std::vector<UpstreamBackend>{ { origin(application) } }, config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            HttpClient old{ ioc, origin(proxy) };
            HttpClient other{ ioc, origin(proxy) };
            bool done{ false };
            bool old_success{ false };
            net::co_spawn(ioc, [&]() -> net::awaitable<void> {
                auto response{ co_await old.AsyncGet("/api/hold") };
                old_success = response && response->AsString() == "A";
                done = true; }, net::detached);
            check(co_await wait_for(a.held, 1), "an old request must be in flight before the new failure");
            static_cast<void>(co_await other.AsyncGet("/api/tag"));
            a.failing.store(true);
            auto failure{ co_await other.AsyncGet("/api/tag") };
            check(failure && failure->Result() == Status::bad_gateway, "a newer request must mark the same backend failed");
            a.failing.store(false);
            a.hold.store(false);
            while (!done) {
                co_await delay(std::chrono::milliseconds{ 5 });
            }
            check(old_success, "the older response must still be delivered successfully");
            for (int i{ 0 }; i < 2; ++i) {
                auto response{ co_await other.AsyncGet("/api/tag") };
                check(response && response->AsString().starts_with("B|"), "a stale successful request must not revive a backend after a newer failure");
            }
            auto truncated{ co_await other.AsyncGet("/body/truncated") };
            check(!truncated, "a truncated upstream body must fail the downstream exchange");
            for (int i{ 0 }; i < 2; ++i) {
                auto response{ co_await other.AsyncGet("/body/tag") };
                check(response && response->AsString().starts_with("B|"), "response-body failures must cool the backend after headers have arrived");
            }
            for (int i{ 0 }; i < 2; ++i) {
                auto response{ co_await other.AsyncGet("/application/tag") };
                check(response && response->Result() == Status::service_unavailable && response->AsString() == "application", "an upstream HTTP error response must remain an application response and not a transport fault");
            }
        });
    }

    /**
     * @brief Apply passive failure and recovery to WebSocket handshakes as well as HTTP.
     */
    auto test_ws_failure_recovery() -> void {
        ControlledUpstream a;
        a.failing.store(true);
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::WsForwardConfig config;
            config.balancing.failure_cooldown = std::chrono::milliseconds{ 100 };
            server.SetWsForward("/ws", std::vector<UpstreamBackend>{ { a.Url(true) }, { origin(b, true) } }, config);
            config.balancing.failure_cooldown = std::chrono::seconds{ 5 };
            server.SetWsForward("/alone", std::vector<UpstreamBackend>{ { a.Url(true) } }, config);
        } };
        run_client([&](net::io_context& ioc) -> net::awaitable<void> {
            WsClient failed{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            boost::system::error_code ec;
            co_await failed.AsyncConnect("/ws", {}, std::chrono::seconds{ 2 }, ec);
            check(static_cast<bool>(ec), "a failed upstream handshake must fail downstream before 101");
            for (int i{ 0 }; i < 2; ++i) {
                WsClient client{ ioc, "127.0.0.1", proxy.Endpoint().port() };
                check((co_await ws_tag(client)).starts_with("B|"), "WebSocket selection must skip cooling backends");
                co_await client.AsyncClose(ec);
            }
            check(a.requests.load() == 1, "a cooling WebSocket backend must not be redialed");
            a.failing.store(false);
            co_await delay(std::chrono::milliseconds{ 130 });
            WsClient recovered{ ioc, "127.0.0.1", proxy.Endpoint().port() };
            check((co_await ws_tag(recovered)) == "A", "a successful recovery handshake must restore the WebSocket backend");
            co_await recovered.AsyncClose(ec);
        });
        a.failing.store(true);
        for (int i{ 0 }; i < 2; ++i) {
            Client client{ proxy.Endpoint() };
            auto const response{
                client.Send(make_request(Verb::get, "/alone", {},
                  { { "Connection", "Upgrade" }, { "Upgrade", "websocket" }, { "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==" }, { "Sec-WebSocket-Version", "13" } }
                  ))
            };
            check(response && response->result() == (i == 0 ? Status::bad_gateway : Status::service_unavailable), "WebSockets must return 503 before 101 when every backend is cooling");
        }
    }

    /**
     * @brief Keep selection and weighted accounting exact across multiple server I/O threads.
     */
    auto test_concurrent_distribution() -> void {
        TestServer a{ [](mhttp::HttpServer& server) { configure_backend(server, "A"); } };
        TestServer b{ [](mhttp::HttpServer& server) { configure_backend(server, "B"); } };
        TestServer proxy{ [&](mhttp::HttpServer& server) {
            mhttp::ReverseProxyConfig config;
            config.balancing.strategy = UpstreamStrategy::WeightedRoundRobin;
            server.SetReverseProxy("/api", std::vector<UpstreamBackend>{
                                               { origin(a), 3 },
                                               { origin(b), 1 }
            },
                                   config);
        } };
        auto const run_worker{ [&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                proxy.ioc.run_for(std::chrono::milliseconds{ 10 });
            }
        } };
        std::jthread second{ run_worker };
        std::jthread third{ run_worker };
        std::atomic<int> a_count{ 0 };
        std::atomic<int> completed{ 0 };
        std::vector<std::jthread> clients;
        for (int worker{ 0 }; worker < 4; ++worker) {
            clients.emplace_back([&] {
                Client client{ proxy.Endpoint() };
                for (int i{ 0 }; i < 16; ++i) {
                    auto const response{ client.Get("/api/tag") };
                    if (response && response->result() == Status::ok) {
                        ++completed;
                        if (response->body().starts_with("A|")) {
                            ++a_count;
                        }
                    }
                }
            });
        }
        clients.clear();
        proxy.server.Stop().get();
        check(completed.load() == 64 && a_count.load() == 48, "parallel session strands must retain the exact 3:1 selection ratio");
    }

    /**
     * @brief Reject invalid groups before registration and leave existing routes usable.
     */
    auto test_configuration() -> void {
        net::io_context ioc;
        mhttp::HttpServer server{ ioc };
        for (auto const& backends : std::vector<std::vector<UpstreamBackend>>{
                 {},
                 { { "http://localhost", 0 } },
                 { { "https://localhost" } },
                 { { "http://localhost?q=1" } },
                 { { "http://localhost", 1 }, { "HTTP://LOCALHOST:80/", 2 } }
        }) {
            bool rejected{ false };
            try {
                server.SetReverseProxy("/api", backends);
            } catch (std::invalid_argument const&) {
                rejected = true;
            }
            check(rejected, "HTTP backend groups must validate empty lists, weights, URLs, and duplicates");
        }
        bool rejected{ false };
        try {
            server.SetWsForward("/ws", std::vector<UpstreamBackend>{ { "http://localhost" } });
        } catch (std::invalid_argument const&) {
            rejected = true;
        }
        check(rejected, "WebSocket backend lists must require ws:// URLs");
        mhttp::ReverseProxyConfig config;
        config.balancing.failure_cooldown = -std::chrono::seconds{ 1 };
        rejected = false;
        try {
            server.SetReverseProxy("/api", std::vector<UpstreamBackend>{ { "http://localhost" } }, config);
        } catch (std::invalid_argument const&) {
            rejected = true;
        }
        check(rejected, "negative failure cooldown must fail registration");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("HTTP distribution and paths", test_http_distribution);
    run_case("WebSocket distribution and paths", test_ws_distribution);
    run_case("HTTP least connections and capacity", test_http_least_and_capacity);
    run_case("WebSocket least connections and stop", test_ws_least_and_stop);
    run_case("pending handshake load", test_pending_handshake_load);
    run_case("failure and recovery", test_failure_and_recovery);
    run_case("single probe and cancellation", test_probe_and_cancellation);
    run_case("failure attribution and stale success", test_failure_attribution);
    run_case("WebSocket failure recovery", test_ws_failure_recovery);
    run_case("concurrent weighted distribution", test_concurrent_distribution);
    run_case("configuration", test_configuration);
    return finish("test_load_balance");
}
