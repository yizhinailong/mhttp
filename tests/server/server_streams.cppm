/**
 * @file server_streams.cppm
 * @brief Verify incremental streaming, EOF compatibility, committed failures, and slow-peer backpressure.
 */
module mhttp:test.server_streams;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Count active provider frames independently of response success or transport closure.
     */
    struct StreamFrameLease {
        std::atomic<unsigned>& active; ///< Counter outliving the listener and every provider.
        /**
         * @brief Observe provider entry.
         * @param count Longer-lived frame counter.
         */
        explicit StreamFrameLease(std::atomic<unsigned>& count) : active{ count } { ++active; }
        /**
         * @brief Observe cleanup after all awaited operations have drained.
         */
        ~StreamFrameLease() { --active; }
    };

    /**
     * @brief Read a bounded head and first body segment while the provider still waits for a test gate.
     * @param peer Owned raw peer, retained through every one-byte read.
     * @param chunked Whether to expect one complete HTTP/1.1 chunk instead of raw HTTP/1.0 bytes.
     * @return Owned prefix, excluding any terminal framing or subsequent response.
     */
    auto receive_stream_prefix(
        std::shared_ptr<asio::ip::tcp::socket> peer,
        bool chunked
    ) -> asio::awaitable<std::string> {
        std::string wire;
        while (wire.size() < 512) {
            char byte{};
            co_await asio::async_read(*peer, asio::buffer(&byte, 1), asio::use_awaitable);
            wire += byte;
            auto const end = wire.find("\r\n\r\n");
            if (end != std::string::npos && std::string_view{ wire }.substr(end + 4)
                == (chunked ? "5\r\nfirst\r\n" : "first")) { co_return wire; }
        }
        throw std::runtime_error{ "stream prefix exceeded its bounded expected size" };
    }

    /**
     * @brief Prove the first body write is visible before provider completion and later pipelines stay ordered.
     * @param executor Independent fixture strand.
     * @param endpoint Dynamic loopback endpoint.
     * @param release Gate allowing the provider to generate its second segment only after the first is received.
     * @param after Number of completed pipeline probe handlers.
     */
    auto server_incremental_stream_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::atomic<bool>& release,
        std::atomic<unsigned>& after
    ) -> asio::awaitable<void> {
        for (bool chunked : { true, false }) {
            release = false;
            auto peer = co_await server_peer(executor, endpoint);
            auto input = std::string{ chunked ? "GET /stream HTTP/1.1\r\n" : "GET /stream HTTP/1.0\r\n" }
                + "Host: local\r\nConnection: keep-alive\r\n\r\n"
                  "GET /after HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
            co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
            auto prefix = co_await receive_stream_prefix(peer, chunked);
            Check(!release.load(), "first body segment reaches the wire before the producer can complete");
            CheckEq(after.load(), chunked ? 0u : 1u, "pipeline cannot dispatch ahead of the active stream");
            release = true;
            auto wire = prefix + co_await receive_server(std::move(peer));
            std::size_t offset{};
            auto stream = parse_session_reply(wire, offset, Method::Get, !chunked);
            CheckEq(stream.body, "firstsecond", "stream writes retain binary order and complete exactly once");
            CheckEq(stream.head.headers.Has("Transfer-Encoding"), chunked, "HTTP/1.0 never advertises transfer coding");
            Check(!stream.head.headers.Has("Content-Length"), "stream never invents a buffered length");
            if (chunked) {
                auto next = parse_session_reply(wire, offset);
                CheckEq(next.body, "after", "HTTP/1.1 pipeline starts after the stream terminator");
            } else { CheckEq(stream.head.headers.Get("Connection"), "close", "HTTP/1.0 stream forces closure despite persistence"); }
            CheckEq(offset, wire.size(), "HTTP/1.0 does not execute buffered successor requests");
        }
        auto edge = co_await server_exchange(executor, endpoint,
            "GET /edge HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        std::size_t offset{};
        auto exact = parse_session_reply(edge, offset);
        CheckEq(exact.body, std::string(505, 'x'), "a chunk whose encoded size equals max_response_bytes succeeds");
        CheckEq(offset, edge.size(), "exact-bound chunk and terminal framing end once");
    }

    /**
     * @brief Run public streamed responses against a gated raw peer without aggregate buffering.
     */
    auto server_incremental_streams() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start incremental stream runtime");
        std::atomic<bool> release{};
        std::atomic<unsigned> after{}, active{};
        server::HttpServer server{ runtime, { .write_timeout = 2s,
            .max_response_bytes = 512, .stream_chunk_bytes = 512 } };
        Check(server.set_http_handler<Method::Get>("/stream", [&](auto&, auto& response) {
            Check(response.SetChunkedContentProvider([&](auto& writer) -> Task<Result<void>> {
                StreamFrameLease lease{ active };
                auto first = co_await writer.Write("first");
                if (!first) { co_return first; }
                while (!release.load()) { co_await Sleep(1ms); }
                co_return co_await writer.Write("second");
            }).has_value(), "configure gated streaming producer");
        }).has_value(), "register incremental stream");
        Check(server.set_http_handler<Method::Get>("/after", [&](auto&, auto& response) {
            ++after;
            Check(response.SetContent("after", "text/plain").has_value(), "pipeline probe content");
        }).has_value(), "register post-stream pipeline probe");
        Check(server.set_http_handler<Method::Get>("/edge", [](auto&, auto& response) {
            Check(response.SetChunkedContentProvider([](auto& writer) -> Task<Result<void>> {
                co_return co_await writer.Write(std::string(505, 'x'));
            }).has_value(), "configure exact framed chunk bound");
        }).has_value(), "register exact wire-bound stream");
        Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start incremental stream listener");
        TransportRuntime peers;
        peers.Run(server_incremental_stream_scenario(peers.Executor(), *server.LocalEndpoint(), release, after));
        Check(server.Stop().has_value(), "drain incremental stream state");
        CheckEq(after.load(), 1u, "only the HTTP/1.1 successor is processed");
        CheckEq(active.load(), 0u, "both streaming frames are released");
    }

    /**
     * @brief Verify committed failures leave one partial response, no terminal chunk, and no pipeline replay.
     * @param executor Fixture strand.
     * @param endpoint Dynamic listener endpoint.
     * @param reset_ready Gate allowing a producer to write only after the peer has reset its socket.
     * @param failures Completed diagnostic count, published after each provider has drained.
     */
    auto server_stream_failure_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::atomic<bool>& reset_ready,
        std::atomic<unsigned>& failures
    ) -> asio::awaitable<void> {
        for (auto mode : { "return", "throw", "wire", "trailer", "terminal", "nominated" }) {
            auto input = std::format("GET /fail/{} HTTP/1.1\r\nHost: local\r\n\r\n"
                "GET /after HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", mode);
            auto wire = co_await server_exchange(executor, endpoint, std::move(input));
            Check(wire.starts_with("HTTP/1.1 200 OK\r\n"), "failure happens after successful head commitment");
            auto const end = wire.find("\r\n\r\n");
            Check(end != std::string::npos && std::string_view{ wire }.substr(end + 4) == "5\r\nfirst\r\n",
                "no failed chunk, terminal bytes, fallback error response, or hidden successor is written");
        }
        auto peer = co_await server_peer(executor, endpoint);
        std::string const input = "GET /fail/reset HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        static_cast<void>(co_await receive_stream_prefix(peer, true));
        peer->set_option(asio::socket_base::linger{ true, 0 });
        peer->close();
        reset_ready = true;
        asio::steady_timer interval{ executor };
        auto const until = std::chrono::steady_clock::now() + 1s;
        while (failures.load() != 7 && std::chrono::steady_clock::now() < until) {
            interval.expires_after(5ms);
            co_await interval.async_wait(asio::use_awaitable);
        }
        CheckEq(failures.load(), 7u, "peer reset aborts the next write and drains well before the response deadline");
    }

    /**
     * @brief Validate framed write bounds and trailers without generating successful terminal framing on failure.
     */
    auto server_failed_streams() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start raw failed stream runtime");
        std::atomic<bool> reset_ready{};
        std::atomic<unsigned> failures{}, after{}, accesses{}, errors{}, posts{}, active{};
        Headers defaults;
        Check(defaults.Add("Connection", "X-Private").has_value(), "connection-nominated trailer name");
        server::HttpServer server{ runtime, { .write_timeout = 2s, .max_response_bytes = 512,
            .default_headers = std::move(defaults), .stream_chunk_bytes = 512 } };
        Check(server.SetLogger([&](auto const& record) {
            if (record.event == server::LogEvent::SessionFailure) { ++failures; }
        }).has_value(), "observe terminal stream failures");
        Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(), "observe complete output only");
        Check(server.SetErrorHandler([&](auto&, auto&) { ++errors; }).has_value(), "observe forbidden error-hook replay");
        Check(server.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "observe single finalization");
        Check(server.set_http_handler<Method::Get>("/fail/:mode", [&](auto& request, auto& response) {
            auto mode = std::string{ request.PathParam("mode") };
            Check(response.SetChunkedContentProvider([&, mode = std::move(mode)](auto& writer) -> Task<Result<void>> {
                StreamFrameLease lease{ active };
                auto first = co_await writer.Write("first");
                if (!first) { co_return first; }
                if (mode == "return") { co_return std::unexpected{ Error::InvalidRequest }; }
                if (mode == "throw") { throw std::runtime_error{ "raw provider failure" }; }
                if (mode == "wire") { co_return co_await writer.Write(std::string(506, 'x')); }
                if (mode == "reset") {
                    while (!reset_ready.load()) { co_await Sleep(1ms); }
                    for (unsigned index{}; index != 100; ++index) {
                        auto written = co_await writer.Write(std::string(128, 'x'));
                        if (!written) { co_return written; }
                    }
                    co_return Result<void>{};
                }
                Headers trailers;
                auto const name = mode == "trailer" ? "Content-Length" : mode == "nominated" ? "X-Private" : "X-Large";
                Check(trailers.Add(name, mode == "terminal" ? std::string(512, 'x') : "1").has_value(), "fixture trailer");
                co_return co_await writer.Finish(std::move(trailers));
            }).has_value(), "defer failing raw producer");
        }).has_value(), "register raw failing stream");
        Check(server.set_http_handler<Method::Get>("/after", [&](auto&, auto&) { ++after; }).has_value(), "register hidden successor probe");
        Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start failed stream listener");
        TransportRuntime peers;
        peers.Run(server_stream_failure_scenario(peers.Executor(), *server.LocalEndpoint(), reset_ready, failures));
        Check(server.Stop().has_value(), "drain failed stream resources");
        Check(after.load() == 0 && accesses.load() == 0 && errors.load() == 0 && posts.load() == 7 && active.load() == 0,
            "committed failures close once, release frames, skip hidden handlers, and do not rerun final/error hooks");
    }

    /**
     * @brief Apply a small receive window and stop reading until the entire-response write deadline fires.
     * @param executor Fixture strand.
     * @param endpoint Dynamic listener endpoint.
     * @param timeouts Published write-timeout diagnostic count.
     */
    auto server_stream_backpressure_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::atomic<unsigned>& timeouts
    ) -> asio::awaitable<void> {
        auto peer = std::make_shared<asio::ip::tcp::socket>(executor);
        peer->open(asio::ip::tcp::v4());
        peer->set_option(asio::socket_base::receive_buffer_size{ 1024 });
        co_await peer->async_connect({ asio::ip::make_address(endpoint.address), endpoint.port }, asio::use_awaitable);
        std::string const input = "GET /large HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        asio::steady_timer interval{ executor };
        auto const until = std::chrono::steady_clock::now() + 2s;
        while (timeouts.load() == 0 && std::chrono::steady_clock::now() < until) {
            interval.expires_after(10ms);
            co_await interval.async_wait(asio::use_awaitable);
        }
        CheckEq(timeouts.load(), 1u, "slow receive window reaches one whole-response write deadline");
        peer->close();
    }

    /**
     * @brief Keep only one bounded owned segment in flight under an unread large streamed response.
     */
    auto server_stream_backpressure() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start stream backpressure runtime");
        std::atomic<unsigned> timeouts{}, completed{}, active{}, accesses{};
        server::HttpServer server{ runtime, { .write_timeout = 100ms,
            .max_response_bytes = 128 * 1024, .stream_chunk_bytes = 64 * 1024 } };
        Check(server.SetLogger([&](auto const& record) {
            if (record.event == server::LogEvent::SessionFailure && record.error == Error::WriteTimeout) { ++timeouts; }
        }).has_value(), "observe streamed write expiry");
        Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(), "reject partial stream access");
        Check(server.set_http_handler<Method::Get>("/large", [&](auto&, auto& response) {
            Check(response.SetChunkedContentProvider([&](auto& writer) -> Task<Result<void>> {
                StreamFrameLease lease{ active };
                for (unsigned index{}; index != 1024; ++index) {
                    auto written = co_await writer.Write(std::string(64 * 1024, 'x'));
                    if (!written) { co_return written; }
                    ++completed;
                }
                co_return Result<void>{};
            }).has_value(), "configure bounded large producer");
        }).has_value(), "register backpressure stream");
        Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start backpressure listener");
        TransportRuntime peers;
        peers.Run(server_stream_backpressure_scenario(peers.Executor(), *server.LocalEndpoint(), timeouts));
        Check(server.Stop().has_value(), "drain timed-out stream");
        Check(completed.load() < 1024 && active.load() == 0 && accesses.load() == 0,
            "provider stops at backpressure instead of buffering the unconsumed response or leaking its frame");
    }

} // namespace mhttp::test
