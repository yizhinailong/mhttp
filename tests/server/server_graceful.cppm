/**
 * @file server_graceful.cppm
 * @brief Verify graceful admission, segmented intake, committed streams, reset, and backpressure with raw peers.
 */
module mhttp:test.server_graceful;

import std;
import asio;
import :core.impl.bridge;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;
import :test.server_streams;

namespace mhttp::test {

    /**
     * @brief Await one cross-thread fixture signal without blocking I/O or polling indefinitely.
     * @param executor Peer strand.
     * @param signal Signal retained through this operation.
     */
    auto grace_signal(asio::any_io_executor executor, std::atomic<bool>& signal) -> asio::awaitable<void> {
        asio::steady_timer timer{ executor };
        auto const until = std::chrono::steady_clock::now() + 1s;
        while (!signal.load() && std::chrono::steady_clock::now() < until) {
            timer.expires_after(1ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
        Check(signal.load(), "request milestone reached before graceful shutdown");
    }

    /**
     * @brief Observe the admission fence without spending the grace budget on an OS connect retry.
     * @param executor Independent peer strand.
     * @param server Borrowed listener retained until its shutdown Task completes.
     */
    auto grace_stopping(asio::any_io_executor executor, server::HttpServer& server) -> asio::awaitable<void> {
        asio::steady_timer timer{ executor };
        auto const until = std::chrono::steady_clock::now() + 1s;
        while (server.IsRunning() && std::chrono::steady_clock::now() < until) {
            timer.expires_after(1ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
        Check(!server.IsRunning(), "grace publishes the admission fence before finishing active requests");
    }

    /**
     * @brief Require both published admission closure and actual connection refusal before proceeding.
     * @param executor Independent peer strand.
     * @param server Borrowed listener retained until its shutdown Task completes.
     * @param endpoint Original bound endpoint.
     */
    auto grace_refused(
        asio::any_io_executor executor,
        server::HttpServer& server,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        asio::steady_timer timer{ executor };
        auto const until = std::chrono::steady_clock::now() + 1s;
        bool refused{};
        while (std::chrono::steady_clock::now() < until) {
            asio::ip::tcp::socket probe{ executor };
            auto [error] = co_await probe.async_connect({ asio::ip::make_address(endpoint.address), endpoint.port },
                asio::as_tuple(asio::use_awaitable));
            if (error && !server.IsRunning()) { refused = true; break; }
            timer.expires_after(1ms);
            co_await timer.async_wait(asio::use_awaitable);
        }
        Check(refused, "grace releases the actual listener");
    }

    /**
     * @brief Finish a body after shutdown starts and reject a buffered successor even when intake is lazy.
     * @param executor Peer strand.
     * @param server Borrowed listener.
     * @param prepared Header/first-body milestone.
     * @param after Successor invocation count.
     */
    auto grace_body_scenario(
        asio::any_io_executor executor,
        server::HttpServer& server,
        std::atomic<bool>& prepared,
        std::atomic<unsigned>& after
    ) -> asio::awaitable<void> {
        auto const endpoint = *server.LocalEndpoint();
        auto peer = co_await server_peer(executor, endpoint);
        std::string input = "POST /upload HTTP/1.1\r\nHost: local\r\nContent-Length: 9\r\n\r\n";
        input += std::string{ "a\0b", 3 };
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        co_await grace_signal(executor, prepared);
        auto stopped = spawn_transport(executor, impl::AwaitTask(server.AsyncStop(2s), executor));
        co_await grace_stopping(executor, server);
        std::string const tail = "cdefghGET /after HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(tail), asio::use_awaitable);
        auto wire = co_await receive_server(peer);
        auto result = co_await stopped->Wait();
        std::size_t offset{};
        auto reply = parse_session_reply(wire, offset);
        Check(result && reply.head.status == Status::Ok && reply.body == std::string{ "a\0bcdefgh", 9 }, "begun body completes after grace starts");
        Check(reply.head.headers.Get("Connection") == "close" && offset == wire.size() && after == 0, "grace closes persistence without dispatching buffered successor");
    }

    /**
     * @brief Verify buffered and lazy body intake finish within grace using fragmented binary requests.
     */
    auto server_grace_bodies() -> void {
        for (bool lazy : { false, true }) {
            Runtime runtime;
            Check(runtime.Start(4).has_value(), "start raw grace body runtime");
            std::atomic<bool> prepared{};
            std::atomic<unsigned> after{};
            server::HttpServer server{ runtime };
            if (lazy) {
                Check(server.RouteLazy(Method::Post, "/upload", [&](auto& request, auto& response) -> Task<void> {
                    std::string body;
                    for (;;) {
                        auto chunk = co_await request.ReadBody();
                        if (!chunk || !*chunk) { break; }
                        body += **chunk;
                        prepared = true;
                    }
                    static_cast<void>(response.SetContent(std::move(body), "application/octet-stream"));
                }).has_value(), "register draining lazy intake");
            } else {
                Check(server.SetPreRoutingHandler([&](auto&, auto&) { prepared = true; return false; }).has_value(), "signal buffered head admission");
                Check(server.Post("/upload", [](auto& request, auto& response) {
                    static_cast<void>(response.SetContent(std::string{ request.Body() }, "application/octet-stream"));
                }).has_value(), "register draining buffered intake");
            }
            Check(server.Get("/after", [&](auto&, auto&) { ++after; }).has_value(), "register hidden pipeline successor");
            Check(server.BindToAnyPort("127.0.0.1") && server.Start() && server.WaitUntilReady(), "start raw body listener");
            TransportRuntime peers;
            peers.Run(grace_body_scenario(peers.Executor(), server, prepared, after));
        }
    }

    /**
     * @brief Retire both a fresh idle peer and an established keep-alive peer without spending grace.
     * @param executor Peer strand.
     * @param server Borrowed listener.
     */
    auto grace_idle_scenario(asio::any_io_executor executor, server::HttpServer& server) -> asio::awaitable<void> {
        auto const endpoint = *server.LocalEndpoint();
        auto persistent = co_await server_peer(executor, endpoint);
        auto fresh = co_await server_peer(executor, endpoint);
        std::string const input = "GET / HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*persistent, asio::buffer(input), asio::use_awaitable);
        auto first = co_await receive_one_server(persistent);
        Check(!first.empty(), "establish idle persistent connection");
        auto const started = std::chrono::steady_clock::now();
        auto stopped = spawn_transport(executor, impl::AwaitTask(server.AsyncStop(2s), executor));
        Check((co_await receive_server(persistent)).empty(), "idle persistent peer receives no synthetic response");
        Check((co_await receive_server(fresh)).empty(), "fresh idle peer closes promptly");
        Check((co_await stopped->Wait()).has_value(), "idle sessions drain naturally");
        Check(std::chrono::steady_clock::now() - started < 1s, "idle sessions do not consume the grace budget");
        co_await grace_refused(executor, server, endpoint);
    }

    /**
     * @brief Verify idle connections close before their ordinary read/keep-alive deadlines.
     */
    auto server_grace_idle() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start idle grace runtime");
        server::HttpServer server{ runtime, { .read_timeout = 5s, .keep_alive_timeout = 5s } };
        Check(server.Get("/", [](auto&, auto& response) { static_cast<void>(response.SetContent("ready", "text/plain")); }).has_value(), "register persistent idle route");
        Check(server.BindToAnyPort("127.0.0.1") && server.Start(), "start idle grace listener");
        TransportRuntime peers;
        peers.Run(grace_idle_scenario(peers.Executor(), server));
    }

    /**
     * @brief Drain a committed stream, or force its provider after a deadline or actual peer reset.
     * @param executor Peer strand.
     * @param server Borrowed listener.
     * @param release Gate allowing the provider to finish or begin a backpressured write.
     * @param mode Zero completes, one parks, two backpressures, three resets the peer.
     * @param fixed Whether the response declares a fixed length.
     * @param http10 Whether the request uses HTTP/1.0 EOF framing.
     */
    auto grace_stream_scenario(
        asio::any_io_executor executor,
        server::HttpServer& server,
        std::atomic<bool>& release,
        unsigned mode,
        bool fixed,
        bool http10
    ) -> asio::awaitable<void> {
        auto const endpoint = *server.LocalEndpoint();
        auto peer = co_await server_peer(executor, endpoint);
        if (mode == 2) { peer->set_option(asio::socket_base::receive_buffer_size{ 1024 }); }
        auto input = std::string{ http10 ? "GET / HTTP/1.0\r\nHost: local\r\nConnection: keep-alive\r\n\r\n"
                                      : "GET / HTTP/1.1\r\nHost: local\r\n\r\n" };
        input += "GET /after HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        auto wire = co_await receive_stream_prefix(peer, !fixed && !http10);
        auto const grace = mode == 1 || mode == 2 ? 40ms : fixed ? 5s : 2s;
        auto stopped = spawn_transport(executor, impl::AwaitTask(server.AsyncStop(grace), executor));
        co_await grace_stopping(executor, server);
        if (fixed) {
            co_await grace_refused(executor, server, endpoint);
            Check(!stopped->value, "actual listener closure precedes the begun response's completion");
        }
        if (mode == 3) {
            peer->set_option(asio::socket_base::linger{ true, 0 });
            peer->close();
        }
        release = true;
        if (mode == 1 || mode == 2) {
            auto result = co_await stopped->Wait();
            Check(!result && result.error() == Error::ShutdownTimeout, "deadline cancels a parked or backpressured stream");
            if (mode == 1) { wire += co_await receive_server(peer); }
            else { peer->close(); }
            Check(wire.starts_with("HTTP/1.1 200") && !wire.ends_with("0\r\n\r\n"), "forced committed stream gets neither terminal chunk nor replacement response");
        } else if (mode == 3) {
            Check((co_await stopped->Wait()).has_value(), "peer reset retires an in-flight stream within grace");
        } else {
            wire += co_await receive_server(peer);
            Check((co_await stopped->Wait()).has_value(), "committed stream completes within grace");
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset, Method::Get, true);
            Check(reply.body == "firstsecond" && offset == wire.size(), "late grace preserves fixed/chunked/EOF framing and rejects the successor");
        }
    }

    /**
     * @brief Run one provider fixture through natural completion, timeout, backpressure, or reset.
     * @param mode Provider outcome selector.
     * @param fixed Whether to declare eleven bytes.
     * @param http10 Whether to use EOF-compatible request framing.
     */
    auto run_grace_stream(unsigned mode, bool fixed = false, bool http10 = false) -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start committed grace stream runtime");
        std::atomic<bool> release{};
        std::atomic<unsigned> after{}, active{};
        server::HttpServer server{ runtime, { .write_timeout = 5s } };
        Check(server.Get("/", [&](auto&, auto& response) {
            server::ContentProvider provider = [&, mode](auto& writer) -> Task<Result<void>> {
                StreamFrameLease frame{ active };
                auto first = co_await writer.Write("first");
                if (!first) { co_return first; }
                while (!release.load()) { co_await Sleep(1ms); }
                if (mode == 1) { co_await Sleep(5s); }
                if (mode == 2) {
                    for (unsigned index{}; index < 8192; ++index) {
                        auto written = co_await writer.Write(std::string(4096, 'x'));
                        if (!written) { co_return written; }
                    }
                }
                co_return co_await writer.Write("second");
            };
            auto configured = fixed ? response.SetContentProvider(11, std::move(provider))
                                    : response.SetChunkedContentProvider(std::move(provider));
            Check(configured.has_value(), "configure owned graceful provider");
        }).has_value(), "register committed grace provider");
        Check(server.Get("/after", [&](auto&, auto&) { ++after; }).has_value(), "register committed stream successor");
        Check(server.BindToAnyPort("127.0.0.1") && server.Start(), "start committed grace listener");
        TransportRuntime peers;
        peers.Run(grace_stream_scenario(peers.Executor(), server, release, mode, fixed, http10));
        Check(after == 0 && active == 0 && !server.LocalEndpoint(), "provider frame and generation drain without invoking successor");
    }

    /**
     * @brief Complete streams after headers have already committed using all supported framing modes.
     */
    auto server_grace_streams() -> void {
        run_grace_stream(0);
        run_grace_stream(0, true);
        run_grace_stream(0, false, true);
    }

    /**
     * @brief Force a parked producer and a slow-reader write, and retire a reset peer without waiting for grace.
     */
    auto server_grace_stream_failures() -> void {
        run_grace_stream(1);
        run_grace_stream(2);
        run_grace_stream(3);
    }

} // namespace mhttp::test
