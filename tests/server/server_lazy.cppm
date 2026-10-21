/**
 * @file server_lazy.cppm
 * @brief Verify lazy intake against segmented raw uploads, early rejection, protocol failures, and stop.
 */
module mhttp:test.server_lazy;

import std;
import asio;
import :server.http_server;
import :core.impl.bridge;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Wait for a handler signal without blocking either runtime or relying on unbounded polling.
     * @param executor Independent peer strand.
     * @param signal Cross-thread flag whose owner outlives this operation.
     */
    auto wait_lazy_signal(
        asio::any_io_executor executor,
        std::atomic<bool>& signal
    ) -> asio::awaitable<void> {
        asio::steady_timer interval{ executor };
        auto const until = std::chrono::steady_clock::now() + 2s;
        while (!signal.load() && std::chrono::steady_clock::now() < until) {
            interval.expires_after(2ms);
            co_await interval.async_wait(asio::use_awaitable);
        }
        Check(signal.load(), "lazy handler progresses before the peer sends the rest of the body");
    }

    /**
     * @brief Send only a body prefix until the handler proves it has decoded it, then complete a pipeline.
     * @param executor Independent peer strand.
     * @param endpoint Dynamic listener address.
     * @param first Signal set after the handler consumes its first owned chunk.
     */
    auto server_lazy_incremental_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::atomic<bool>& first
    ) -> asio::awaitable<void> {
        for (bool chunked : { false, true }) {
            first = false;
            auto peer = co_await server_peer(executor, endpoint);
            auto head = std::string{ "POST /read/42 HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\n" }
                + (chunked ? "Transfer-Encoding: chunked\r\nTrailer: X-Sum\r\n\r\n" : "Content-Length: 9\r\n\r\n");
            co_await asio::async_write(*peer, asio::buffer(head), asio::use_awaitable);
            std::string interim(25, '\0');
            co_await asio::async_read(*peer, asio::buffer(interim), asio::use_awaitable);
            CheckEq(interim, "HTTP/1.1 100 Continue\r\n\r\n", "first lazy read releases an Expect upload");
            auto prefix = std::string{ "a\0b", 3 };
            if (chunked) { prefix = "3\r\n" + prefix + "\r\n"; }
            co_await asio::async_write(*peer, asio::buffer(prefix), asio::use_awaitable);
            co_await wait_lazy_signal(executor, first);
            auto suffix = std::string{ chunked ? "6\r\ncdefgh\r\n0\r\nX-Sum: 9\r\n\r\n" : "cdefgh" }
                + "GET /after HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
            co_await asio::async_write(*peer, asio::buffer(suffix), asio::use_awaitable);
            auto wire = co_await receive_server(peer);
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.body, std::string("a\0bcdefgh", 9), "segmented binary upload is decoded without buffering ahead");
            CheckEq(reply.head.headers.Get("X-Sum"), chunked ? "9" : "", "trailers appear only after complete decoding");
            CheckEq(parse_session_reply(wire, offset).body, "after", "retained pipeline suffix becomes exactly one next request");
            CheckEq(offset, wire.size(), "no duplicate interim or final response");
        }
        auto empty = co_await server_exchange(executor, endpoint,
            "POST /empty HTTP/1.1\r\nHost: local\r\nContent-Length: 0\r\n\r\n"
            "GET /after HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        std::size_t offset{};
        CheckEq(parse_session_reply(empty, offset).body, "empty", "declared empty lazy body requires no read");
        CheckEq(parse_session_reply(empty, offset).body, "after", "empty framing permits immediate persistence");
        CheckEq(offset, empty.size(), "empty request does not consume pipeline bytes");
    }

    /**
     * @brief Exercise bounded reads with tiny parser feeds, ownership across awaits, trailers, and persistence.
     */
    auto server_lazy_incremental() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start incremental lazy runtime");
        std::atomic<bool> first{};
        std::atomic<unsigned> after{}, posts{};
        server::HttpServer server{ runtime, { .body_bytes = 9, .request_chunk_bytes = 2 } };
        Check(server.RouteLazy(Method::Post, "/read/:id", [&](auto& request, auto& response) -> Task<void> {
            CheckEq(request.template PathParam<unsigned>("id"), std::optional{ 42u }, "lazy captures initialized before body");
            std::string body;
            std::optional<std::string> retained;
            for (;;) {
                auto chunk = co_await request.ReadBody();
                Check(chunk.has_value(), "decode segmented lazy body");
                if (!*chunk) { break; }
                Check((**chunk).size() <= 2 && request.Body().empty(), "bounded chunk with no aggregate body");
                if (!retained) { retained = **chunk; }
                body += **chunk;
                first = true;
                co_await Sleep(1ms);
            }
            Check(body.starts_with(*retained), "first chunk owns bytes across subsequent reads");
            Check(!request.Headers().Has("X-Sum"), "trailers do not mutate initial fields");
            Check(response.Headers().Set("X-Sum", request.Trailers().Get("X-Sum")).has_value(), "publish decoded trailer");
            Check(response.SetContent(std::move(body), "application/octet-stream").has_value(), "echo decoded binary upload");
        }).has_value(), "register segmented lazy upload");
        Check(server.RouteLazy(Method::Post, "/empty", [](auto&, auto& response) {
            Check(response.SetContent("empty", "text/plain").has_value(), "reply without reading empty framing");
        }).has_value(), "register empty lazy request");
        Check(server.Get("/after", [&](auto&, auto& response) {
            ++after;
            Check(response.SetContent("after", "text/plain").has_value(), "pipeline successor");
        }).has_value(), "register successor");
        Check(server.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "count final hooks only");
        Check(server.BindToAnyPort("127.0.0.1") && server.Start(), "start segmented upload listener");
        TransportRuntime peers;
        peers.Run(server_lazy_incremental_scenario(peers.Executor(), *server.LocalEndpoint(), first));
        Check(server.Stop() && after == 3 && posts == 6, "every complete request has one final hook");
    }

    /**
     * @brief Reject one upload through ordinary global authentication before any body read or interim response.
     */
    struct LazyAuthentication {
        /**
         * @brief Refuse the protected path using only admitted metadata.
         * @param request Borrowed request before body intake.
         * @param response Borrowed response for immediate rejection.
         * @return Whether to invoke the selected route.
         */
        auto Before(server::Request& request, server::Response& response) -> bool {
            if (request.Path() != "/auth") { return true; }
            static_cast<void>(response.SetError(Status::Unauthorized));
            return false;
        }
        /**
         * @brief Mark both ordinary and rejected route completion.
         * @param request Unused borrowed metadata.
         * @param response Borrowed response.
         * @return Continue through remaining middleware.
         */
        auto After(server::Request& request, server::Response& response) -> bool {
            static_cast<void>(request);
            static_cast<void>(response.Headers().Set("X-After", "yes"));
            return true;
        }
    };

    /**
     * @brief Verify no-read, partial-read, and middleware rejections do not drain or dispatch successor bytes.
     * @param executor Independent peer strand.
     * @param endpoint Dynamic listener endpoint.
     */
    auto server_lazy_rejection_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (auto path : { "/none", "/partial", "/auth" }) {
            auto input = std::format("POST {} HTTP/1.1\r\nHost: local\r\nContent-Length: 8\r\n{}\r\n", path,
                path == std::string_view{ "/partial" } ? "" : "Expect: 100-continue\r\n");
            if (path == std::string_view{ "/partial" }) { input += "abcdefgh"; }
            input += "GET /after HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
            auto wire = co_await server_exchange(executor, endpoint, std::move(input));
            Check(!wire.contains("100 Continue"), "no-read rejection suppresses interim response");
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, path == std::string_view{ "/auth" } ? Status::Unauthorized : Status::Ok,
                "early final response remains customizable");
            CheckEq(reply.head.headers.Get("Connection"), "close", "unconsumed input prohibits persistence");
            CheckEq(reply.head.headers.Get("X-After"), "yes", "global After still runs on early rejection");
            CheckEq(offset, wire.size(), "unread upload and pipeline bytes never dispatch a second request");
        }
    }

    /**
     * @brief Preserve authentication before lazy intake and close after successful partial reads.
     */
    auto server_lazy_rejection() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start rejected lazy runtime");
        std::atomic<unsigned> forbidden{}, after{}, posts{};
        server::HttpServer server{ runtime, { .request_chunk_bytes = 2 } };
        Check(server.Use(LazyAuthentication{}).has_value(), "authenticate before reads");
        Check(server.RouteLazy(Method::Post, "/none", [](auto&, auto& response) {
            Check(response.SetContent("none", "text/plain").has_value(), "reply without reading upload");
        }).has_value(), "register no-read route");
        Check(server.RouteLazy(Method::Post, "/partial", [](auto& request, auto& response) -> Task<void> {
            auto prefix = co_await request.ReadBody();
            Check(prefix && *prefix && **prefix == "ab", "partial read consumes only one bounded chunk");
            Check(response.SetContent("partial", "text/plain").has_value(), "reply without draining remainder");
        }).has_value(), "register partial-read route");
        Check(server.RouteLazy(Method::Post, "/auth", [&](auto&, auto&) { ++forbidden; }).has_value(), "protected route");
        Check(server.Get("/after", [&](auto&, auto&) { ++after; }).has_value(), "forbidden pipeline successor");
        Check(server.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "observe early final hooks");
        Check(server.BindToAnyPort("127.0.0.1") && server.Start(), "start rejecting upload listener");
        TransportRuntime peers;
        peers.Run(server_lazy_rejection_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop() && forbidden == 0 && after == 0 && posts == 3, "rejection preserves routing boundaries");
    }

    /**
     * @brief Test fatal input after an admitted head, including half-close and one whole-body timeout.
     * @param executor Independent peer strand.
     * @param endpoint Dynamic listener endpoint.
     */
    auto server_lazy_failures_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        std::array<std::pair<std::string, Status>, 6> inputs{{
            { "Content-Length: 6\r\n\r\n", Status::ContentTooLarge },
            { "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n", Status::ContentTooLarge },
            { "Transfer-Encoding: chunked\r\n\r\nx\r\n", Status::BadRequest },
            { "Content-Length: 5\r\n\r\nab", Status::BadRequest },
            { "Transfer-Encoding: chunked\r\n\r\n2\r\na", Status::BadRequest },
            { "Content-Length: 5\r\n\r\n", Status::RequestTimeout },
        }};
        for (std::size_t index{}; index != inputs.size(); ++index) {
            auto peer = co_await server_peer(executor, endpoint);
            auto input = "POST /read HTTP/1.1\r\nHost: local\r\n" + inputs[index].first;
            co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
            if (index == 3 || index == 4) { peer->shutdown(asio::ip::tcp::socket::shutdown_send); }
            auto wire = co_await receive_server(peer);
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, inputs[index].second, "sticky intake failure overrides handler success");
            CheckEq(reply.head.headers.Get("Connection"), "close", "every failed intake closes");
            CheckEq(reply.head.headers.Get("X-Error"), "once", "input failure enters error hook");
            CheckEq(reply.head.headers.Get("X-Post"), "once", "input failure enters post hook");
            CheckEq(offset, wire.size(), "exactly one error response");
        }
        auto peer = co_await server_peer(executor, endpoint);
        std::string input{ "POST /read HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n" };
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        asio::steady_timer interval{ executor };
        interval.expires_after(60ms);
        co_await interval.async_wait(asio::use_awaitable);
        input = "1\r\nb\r\n";
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        auto wire = co_await receive_server(peer);
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).head.status, Status::RequestTimeout,
            "receiving another valid chunk does not extend the body phase deadline");
    }

    /**
     * @brief Run error and post hooks once after ignored parser/transport failures, with total input bounds intact.
     */
    auto server_lazy_failures() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start failed lazy runtime");
        std::atomic<unsigned> handlers{}, errors{}, posts{};
        server::HttpServer server{ runtime, { .read_timeout = 100ms, .body_bytes = 5, .request_chunk_bytes = 2 } };
        Check(server.RouteLazy(Method::Post, "/read", [&](auto& request, auto& response) -> Task<void> {
            ++handlers;
            for (;;) {
                auto chunk = co_await request.ReadBody();
                if (!chunk || !*chunk) { break; }
            }
            Check(response.SetContent("ignored error", "text/plain").has_value(), "attempt to hide intake failure");
        }).has_value(), "register failed lazy reader");
        Check(server.SetErrorHandler([&](auto&, auto& response) {
            ++errors;
            Check(response.Headers().Set("X-Error", "once").has_value(), "customize fatal request response");
        }).has_value(), "install lazy input error hook");
        Check(server.SetPostRoutingHandler([&](auto&, auto& response) {
            ++posts;
            Check(response.Headers().Set("X-Post", "once").has_value(), "finalize fatal request response");
        }).has_value(), "install lazy input final hook");
        Check(server.BindToAnyPort("127.0.0.1") && server.Start(), "start failed upload listener");
        TransportRuntime peers;
        peers.Run(server_lazy_failures_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop() && handlers == 6 && errors == 7 && posts == 7, "head rejection skips lazy handler; final hooks run once");
    }

    /**
     * @brief Hold an incomplete upload until cancellation, then await complete listener drain on an independent runtime.
     * @param executor Independent peer strand.
     * @param endpoint Dynamic listener endpoint.
     * @param entered Signal that the route is waiting in ReadBody.
     * @param server Listener kept alive through asynchronous stop.
     * @param runtime Server runtime to cancel in the second variation.
     * @param stop_runtime Whether to cancel its runtime generation before stopping the listener.
     */
    auto server_lazy_stop_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::atomic<bool>& entered,
        server::HttpServer& server,
        Runtime& runtime,
        bool stop_runtime
    ) -> asio::awaitable<void> {
        auto peer = co_await server_peer(executor, endpoint);
        std::string head{ "POST /wait HTTP/1.1\r\nHost: local\r\nContent-Length: 10\r\n\r\n" };
        co_await asio::async_write(*peer, asio::buffer(head), asio::use_awaitable);
        co_await wait_lazy_signal(executor, entered);
        asio::steady_timer interval{ executor };
        interval.expires_after(20ms);
        co_await interval.async_wait(asio::use_awaitable);
        if (stop_runtime) { runtime.Stop(); }
        co_await impl::AwaitTask(server.AsyncStop(), executor);
        Check((co_await receive_server(peer)).empty(), "explicit cancellation emits no error response");
    }

    /**
     * @brief Cancel an actual socket read and release every handler frame on listener and runtime stop.
     */
    auto server_lazy_stop() -> void {
        for (bool stop_runtime : { false, true }) {
            Runtime runtime;
            Check(runtime.Start(4).has_value(), "start canceled lazy runtime");
            std::atomic<bool> entered{};
            std::atomic<unsigned> active{}, posts{}, accesses{};
            server::HttpServer server{ runtime };
            Check(server.RouteLazy(Method::Post, "/wait", [&](auto& request, auto&) -> Task<void> {
                ++active;
                /**
                 * @brief Observe route frame cleanup after cancellation propagation.
                 */
                struct FrameLease {
                    std::atomic<unsigned>& count; ///< Counter outliving every route frame.
                    /**
                     * @brief Release the live-frame count on every exit path.
                     */
                    ~FrameLease() { --count; }
                } lease{ active };
                entered = true;
                static_cast<void>(co_await request.ReadBody());
            }).has_value(), "register suspended lazy read");
            Check(server.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "observe canceled final hooks");
            Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(), "observe canceled responses");
            Check(server.BindToAnyPort("127.0.0.1") && server.Start(), "start canceled upload listener");
            TransportRuntime peers;
            peers.Run(server_lazy_stop_scenario(peers.Executor(), *server.LocalEndpoint(), entered, server, runtime, stop_runtime));
            Check(server.Stop() && active == 0 && posts == 0 && accesses == 0, "canceled intake drains without finalization or leaks");
        }
    }

} // namespace mhttp::test
