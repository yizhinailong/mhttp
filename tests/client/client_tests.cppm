/**
 * @file client_tests.cppm
 * @brief Validate public client framing, malicious responses, disconnects, and actual read/write deadlines.
 */
export module mhttp:test.client;

import std;
import asio;
import :client.http_client;
import :server.http_server;
import :test.transport;
import :test.client_peer;
import :test.client_write_fault;
import :test.client_expect;
import :test.client_streaming;
import :test.client_providers;

namespace mhttp::test {

    using namespace std::chrono_literals;

    /**
     * @brief Parse fixed, chunked, interim, EOF-delimited, HEAD, and bodyless status responses over real sockets.
     */
    auto client_response_framing() -> void {
        auto fixed = run_client_peer({ "HTTP/1.1 200 Custom reason\r\nContent-Length: 3\r\nX-Repeat: a\r\nx-repeat: b\r\n\r\nx", std::string{ "\0y", 2 }, 10ms });
        Check(fixed.response && fixed.response->Body() == std::string_view{ "x\0y", 3 }, "split fixed body preserves embedded NUL");
        if (fixed.response) {
            CheckEq(fixed.response->Reason(), "Custom reason", "client retains exact received reason");
            CheckEq(fixed.response->Headers().Values("X-Repeat"), std::vector<std::string_view>{ "a", "b" }, "duplicate response fields preserve wire order");
        }
        auto chunked = run_client_peer({ "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n"
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-Sum\r\n\r\n3;ext=yes\r\na", "bc\r\n2\r\nde\r\n0\r\nX-Sum: 42\r\n\r\n", 10ms });
        Check(chunked.response && chunked.response->Body() == "abcde", "skip interim messages and decode fragmented chunks");
        if (chunked.response) {
            CheckEq(chunked.response->Trailers().Get("X-Sum"), "42", "client owns separate validated trailers");
            Check(!chunked.response->Headers().Has("X-Sum"), "trailers are not merged into ordinary response fields");
        }
        for (auto version : { "HTTP/1.0", "HTTP/1.1" }) {
            auto eof = run_client_peer({ std::string{ version } + " 200 OK\r\n\r\nclose-delimited" });
            Check(eof.response && eof.response->Body() == "close-delimited" && !eof.response->KeepAlive() && !eof.open,
                "read-to-close body completes normally and discards connection");
        }
        for (auto status : { "204 No Content", "304 Not Modified", "404 Not Found" }) {
            auto response = run_client_peer({ "HTTP/1.1 " + std::string{ status } + "\r\nContent-Length: 0\r\n\r\n" });
            Check(response.response && response.response->Body().empty(), "bodyless/error status is a successful response");
        }
        auto head = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\n" }, {}, client::Request{ Method::Head, "/" });
        Check(head.response && head.response->Body().empty(), "HEAD never waits for advertised representation bytes");
    }

    /**
     * @brief Distinguish truncated fixed/chunked messages from successful close-delimited input.
     */
    auto client_truncated_responses() -> void {
        for (auto wire : { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nab",
                           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab",
                           "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n",
                           "HTTP/1.1 100 Continue\r\n\r\n" }) {
            auto result = run_client_peer({ wire });
            Check(!result.response && result.response.error() == Error::ConnectionClosed && !result.open,
                "premature EOF reports ConnectionClosed and discards all incomplete state");
        }
        auto bad = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n" });
        Check(!bad.response && bad.response.error() == Error::ProtocolError && !bad.open, "ambiguous response framing is rejected");
        auto extra = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\nunsolicited" });
        Check(!extra.response && extra.response.error() == Error::ProtocolError && !extra.open, "unsolicited bytes cannot poison a successor response");
    }

    /**
     * @brief Apply decoded body limits to every framing mode and shared header/trailer admission bounds.
     */
    auto client_response_limits() -> void {
        client::ClientOptions options{ .body_bytes = 4 };
        for (auto wire : { "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n12345",
                           "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n12345\r\n0\r\n\r\n",
                           "HTTP/1.1 200 OK\r\n\r\n12345" }) {
            auto result = run_client_peer({ wire }, options);
            Check(!result.response && result.response.error() == Error::BodyTooLarge && !result.open, "every body framing mode enforces the same decoded bound");
        }
        auto exact = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n1234" }, options);
        Check(exact.response && exact.response->Body() == "1234", "exact decoded body bound is accepted");
        options.header_bytes = 24;
        auto bytes = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-Long: too-long\r\n\r\n" }, options);
        Check(!bytes.response && bytes.response.error() == Error::HeaderTooLarge && !bytes.open, "response header byte bound is applied to actual reads");
        options.header_bytes = 1024;
        options.header_fields = 1;
        auto fields = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-Extra: 1\r\n\r\n" }, options);
        Check(!fields.response && fields.response.error() == Error::HeaderTooLarge, "response field count bound is enforced");
        auto trailers = run_client_peer({ "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX-Extra: 1\r\n\r\n" }, options);
        Check(!trailers.response && trailers.response.error() == Error::HeaderTooLarge, "trailers share the response field count budget");
    }

    /**
     * @brief Reject known unsupported compression while preserving unknown and identity representations exactly.
     */
    auto client_content_encoding() -> void {
        for (auto coding : { "gzip", "GZip", "deflate", "br", "zstd", "identity, gzip" }) {
            auto result = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Encoding: " + std::string{ coding } + "\r\nContent-Length: 3\r\n\r\nraw" });
            Check(!result.response && result.response.error() == Error::UnsupportedContentEncoding && !result.open,
                "advertised built-in decoder requirement is reported explicitly");
        }
        for (auto coding : { "identity", "custom-codec", "unknown, identity" }) {
            auto result = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Encoding: " + std::string{ coding } + "\r\nContent-Length: 3\r\n\r\nraw" });
            Check(result.response && result.response->Body() == "raw" && result.response->Header("Content-Encoding") == coding,
                "unknown/identity coding bytes and metadata pass through unchanged");
        }
    }

    /**
     * @brief Trigger actual stalled-body reads and nonreading-peer writes under bounded phase deadlines.
     */
    auto client_phase_deadlines() -> void {
        client::ClientOptions read_options{ .read_timeout = 50ms };
        auto stalled = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab", {}, 200ms }, read_options);
        Check(!stalled.response && stalled.response.error() == Error::ReadTimeout && !stalled.open, "mid-body stall hits the response deadline");
        Check(stalled.elapsed >= 25ms && stalled.elapsed < 180ms, "stalled read returns before peer closure");
        client::ClientOptions write_options{ .write_timeout = 100ms };
        client::Request upload{ Method::Post, "/upload" };
        Check(upload.SetContent(std::string(8 * 1024 * 1024, 'x')).has_value(), "bounded large upload");
        auto blocked = run_client_peer({ {}, {}, 350ms, false }, write_options, std::move(upload));
        Check(!blocked.response && blocked.response.error() == Error::WriteTimeout && !blocked.open, "nonreading peer triggers a real whole-request write deadline");
        Check(blocked.elapsed >= 50ms && blocked.elapsed < 300ms, "blocked write returns before peer closes without waiting for a response");
    }

    /**
     * @brief Enforce a two-second overall deadline despite body fragments arriving every 150 ms.
     * @param framing One of cl, eof, or chunked, selecting independent raw wire framing.
     */
    auto client_overall_deadline(std::string_view framing) -> void {
        bool const chunked = framing == "chunked";
        ClientPeerPlan plan{ "HTTP/1.1 200 OK\r\n" };
        if (framing == "cl") { plan.first += "Content-Length: 20\r\n"; }
        if (chunked) { plan.first += "Transfer-Encoding: chunked\r\n"; }
        plan.first += chunked ? "\r\n1\r\nx\r\n" : "\r\nx";
        for (unsigned i{}; i < 19; ++i) { plan.steps.emplace_back(150ms, chunked ? "1\r\nx\r\n" : "x"); }
        if (chunked) { plan.steps.emplace_back(150ms, "0\r\n\r\n"); }
        auto result = run_client_peer(std::move(plan), { .timeout_policy = client::TimeoutPolicy::Overall, .timeout = 2s });
        Check(!result.response && result.response.error() == Error::ReadTimeout && !result.open, "overall deadline closes incomplete response despite continuing progress");
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(result.elapsed);
        Check(elapsed >= 2000ms && elapsed < 2200ms, std::format("{} overall duration {} ms is within [2000, 2200)", framing, elapsed.count()));
    }

    /**
     * @brief Renew Step timers on progress, disable Never timers, and retain real write deadline classification.
     */
    auto client_timeout_policies() -> void {
        ClientPeerPlan drip{ "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n\r\nx" };
        for (unsigned i{}; i < 6; ++i) { drip.steps.emplace_back(40ms, "x"); }
        auto stepped = run_client_peer(std::move(drip),
            { .read_timeout = 1ms, .timeout_policy = client::TimeoutPolicy::Step, .timeout = 100ms });
        Check(stepped.response && stepped.response->Body() == "xxxxxxx" && stepped.elapsed >= 200ms, "Step renews after each read and overrides independent phase timeout");
        auto stalled = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nx", "x", 250ms },
            { .timeout_policy = client::TimeoutPolicy::Step, .timeout = 100ms });
        Check(!stalled.response && stalled.response.error() == Error::ReadTimeout && stalled.elapsed < 220ms, "Step expires when one read stops making progress");
        auto untimed = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nx", "y", 200ms },
            { .connect_timeout = 1ms, .read_timeout = 1ms, .write_timeout = 1ms,
                .timeout_policy = client::TimeoutPolicy::Never, .timeout = 1ms });
        Check(untimed.response && untimed.response->Body() == "xy" && untimed.elapsed >= 200ms, "Never disables all automatic phase and per-operation deadlines");
        client::Request upload{ Method::Post, "/upload" };
        Check(upload.SetContent(std::string(8 * 1024 * 1024, 'x')).has_value(), "large overall-budget upload");
        auto blocked = run_client_peer({ {}, {}, 350ms, false },
            { .timeout_policy = client::TimeoutPolicy::Overall, .timeout = 100ms }, std::move(upload));
        Check(!blocked.response && blocked.response.error() == Error::WriteTimeout && blocked.elapsed < 300ms, "overall budget also expires during blocked request writes");
    }

    /**
     * @brief Reject connection establishment to an authoritative closed dynamic loopback port.
     */
    auto client_connection_refused() -> void {
        TransportRuntime peer_runtime;
        asio::ip::tcp::acceptor listener{ peer_runtime.Executor(), { asio::ip::address_v4::loopback(), 0 } };
        auto const port = listener.local_endpoint().port();
        listener.close();
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start refused-connection runtime");
        client::HttpClient client{ runtime, "127.0.0.1", port };
        auto refused = SyncWait(runtime, client.Send(client::Request{}));
        Check(!refused && refused.error() == Error::Connection && !client.IsOpen(), "refused dynamic loopback endpoint reports Connection");
    }

    /**
     * @brief Send actual IPv6 requests with bracketed Host fields, skipping only unavailable address families.
     */
    auto client_ipv6_loopback() -> void {
        TransportRuntime probe_runtime;
        asio::ip::tcp::acceptor probe{ probe_runtime.Executor() };
        asio::error_code error;
        probe.open(asio::ip::tcp::v6(), error);
        if (!error) { probe.bind({ asio::ip::address_v6::loopback(), 0 }, error); }
        if (error) {
            if (error == asio::error::address_family_not_supported || error == asio::error::operation_not_supported
                || error == std::errc::address_not_available) {
                std::cout << "[skip] IPv6 loopback is unavailable: " << error.message() << '\n';
                return;
            }
            throw std::system_error{ error };
        }
        probe.close();
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start IPv6 client runtime");
        server::HttpServer server{ runtime };
        Check(server.set_http_handler<Method::Get>("/", [](auto& request, auto& response) {
            CheckEq(request.RemoteEndpoint().address, std::string{ "::1" }, "actual IPv6 peer endpoint");
            Check(response.Headers().Set("X-Host", request.Header("Host")).has_value(), "echo IPv6 Host authority");
        }).has_value(), "register IPv6 echo");
        auto port = server.BindToAnyPort("::1");
        Check(port && server.Start() && server.WaitUntilReady(), "start IPv6 dynamic-port listener");
        if (!port) { return; }
        auto const authority = "[::1]:" + std::to_string(*port);
        for (auto host : { "::1", "[::1]" }) {
            client::HttpClient client{ runtime, host, *port };
            auto result = SyncWait(runtime, client.Get());
            Check(result && result->Header("X-Host") == authority && result->RemoteEndpoint().address == "::1",
                "bare and bracketed IPv6 constructors connect and format Host correctly");
        }
        client::HttpClient client{ runtime, "http://" + authority + "/#local" };
        auto result = SyncWait(runtime, client.Get());
        Check(result && result->Header("X-Host") == authority && result->LocalEndpoint().address == "::1",
            "IPv6 URL constructor owns bracketed authority and uses IPv6 transport");
    }

    /**
     * @brief Exercise the actual Send state machine with deterministic head completions and real HTTP peers.
     */
    auto client_head_retry_budget() -> void {
        using namespace std::string_literals;

        /**
         * @brief Describe one retry precondition and the complete expected attempt budget.
         */
        struct Case {
            bool reused; ///< Establish a real persistent connection first.
            std::vector<ClientWriteFailure> failures; ///< Scripted head failures, retaining real subsequent I/O.
            std::size_t attempts; ///< Required total attempts for the upload head.
            std::optional<Error> error; ///< Expected terminal failure, absent for a complete upload.
        };
        std::vector<Case> const cases{
            { true, { {} }, 2, {} },
            { true, { {}, {}, {} }, 2, Error::ConnectionClosed },
            { false, { {} }, 1, Error::ConnectionClosed },
            { true, { { Error::ConnectionClosed, 1 } }, 1, Error::ConnectionClosed },
            { true, { { Error::WriteTimeout } }, 1, Error::WriteTimeout },
            { true, { { Error::Canceled } }, 1, Error::Canceled },
            { true, { { Error::Write } }, 1, Error::Write },
        };
        for (auto const& test : cases) {
            Runtime runtime;
            Check(runtime.Start(4).has_value(), "start scripted retry runtime");
            server::HttpServer server{ runtime };
            std::atomic<unsigned> uploads{};
            std::mutex body_mutex;
            std::string observed;
            Check(server.set_http_handler<Method::Get>("/warmup", [](auto&, auto&) {}).has_value(), "register persistent warmup");
            Check(server.set_http_handler<Method::Post>("/retry", [&](auto& request, auto& response) {
                ++uploads;
                std::scoped_lock lock{ body_mutex };
                observed = request.Body();
                Check(response.SetContent("stored", "text/plain").has_value(), "acknowledge one full upload");
            }).has_value(), "register real upload peer");
            auto port = server.BindToAnyPort("127.0.0.1");
            if (!port || !server.Start() || !server.WaitUntilReady()) {
                throw std::runtime_error{ "cannot start scripted retry peer" };
            }
            client::HttpClient client{ runtime, "127.0.0.1", *port };
            if (test.reused) {
                auto warmup = SyncWait(runtime, client.Get("/warmup"));
                Check(warmup && warmup->KeepAlive() && client.IsOpen(), "fault starts on a known persistent socket");
            }
            ClientWriteFault::Configure(test.failures);
            client::Request request{ Method::Post, "/retry" };
            auto const payload = "owned\0upload"s;
            Check(request.SetContent(payload).has_value(), "retain a binary non-idempotent upload");
            auto result = SyncWait(runtime, client.Send(std::move(request)));
            CheckEq(ClientWriteFault::Calls(), test.attempts, "head retry budget and transfer classification");
            if (test.error) {
                Check(!result && result.error() == *test.error && !client.IsOpen(), "terminal failure closes the transport");
            } else {
                Check(result && result->Body() == "stored", "zero-byte stale write reconnects and completes the upload");
            }
            Check(server.Stop().has_value(), "drain every failed or successful retry connection");
            CheckEq(uploads.load(), test.error ? 0U : 1U, "upload dispatch occurs exactly once only after head success");
            std::scoped_lock lock{ body_mutex };
            CheckEq(observed, test.error ? std::string{} : payload, "retry preserves every owned body byte");
        }
    }

    /**
     * @brief Never transparently replay a non-idempotent request whose head has already completed a prefix write.
     */
    auto client_partial_head_no_retry() -> void {
        TransportRuntime peer_runtime;
        auto listener = std::make_shared<asio::ip::tcp::acceptor>(peer_runtime.Executor(),
            asio::ip::tcp::endpoint{ asio::ip::address_v4::loopback(), 0 });
        auto const port = listener->local_endpoint().port();
        unsigned connections{};
        std::exception_ptr failure;
        std::jthread worker{ [&] {
            try { peer_runtime.Run(reset_client_head_prefix(listener, connections)); }
            catch (...) { failure = std::current_exception(); }
        } };
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start partial-head client runtime");
        client::HttpClient client{ runtime, "127.0.0.1", port, { .write_timeout = 500ms } };
        Check(SyncWait(runtime, client.Get()).has_value(), "establish the persistent socket before resetting its successor head");
        client::Request request{ Method::Post, "/large" };
        Check(request.Headers().Set("X-Large", std::string(8 * 1024 * 1024, 'h')).has_value(), "bounded large request head spans many transfer completions");
        Check(request.SetContent("non-idempotent-body").has_value(), "body stays owned until head transfer completes");
        auto result = SyncWait(runtime, client.Send(std::move(request)));
        Check(!result && result.error() == Error::ConnectionClosed && !client.IsOpen(), "head prefix reset reports failure instead of replaying the request");
        worker.join();
        if (failure) { std::rethrow_exception(failure); }
        CheckEq(connections, 1U, "peer observes no second TCP connection or request replay after a completed head prefix");
    }

} // namespace mhttp::test

export namespace mhttp::test {

    /**
     * @brief Run private raw-peer client acceptance without exporting fixtures from production.
     * @param argc Argument count.
     * @param argv Optional case filters.
     * @return Test runner status.
     */
    auto RunClientTests(int argc, char* argv[]) -> int {
        return RunTests(argc, argv, {
            { "client streamed raw framing", client_streamed_raw_framing },
            { "client streamed raw failures", client_streamed_raw_failures },
            { "client streamed callback policies", client_streamed_callback_policies },
            { "client provider head failures", client_provider_head_failures },
            { "client provider backpressure", client_provider_backpressure },
            { "client provider Expect fallback", client_provider_expect_fallback },
            { "client provider peer reset", client_provider_peer_reset },
            { "client response framing", client_response_framing },
            { "client truncated responses", client_truncated_responses },
            { "client response limits", client_response_limits },
            { "client content encoding", client_content_encoding },
            { "client phase deadlines", client_phase_deadlines },
            { "client overall CL deadline", [] { client_overall_deadline("cl"); } },
            { "client overall EOF deadline", [] { client_overall_deadline("eof"); } },
            { "client overall chunked deadline", [] { client_overall_deadline("chunked"); } },
            { "client timeout policies", client_timeout_policies },
            { "client connection refused", client_connection_refused },
            { "client IPv6 loopback", client_ipv6_loopback },
            { "client partial head no retry", client_partial_head_no_retry },
            { "client head retry budget", client_head_retry_budget },
            { "client Expect handshake", client_expect_handshake },
            { "client Expect fallback", client_expect_fallback },
            { "client Expect early final", client_expect_early_final },
            { "client Expect deadlines", client_expect_deadlines },
            { "client Expect cancellation", client_expect_cancellation },
            { "client Expect invalid interims", client_expect_invalid_interims },
            { "client Expect write deadline", client_expect_write_deadline },
        });
    }

} // namespace mhttp::test
