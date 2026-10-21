/**
 * @file client_tests.cppm
 * @brief Validate public client framing, malicious responses, disconnects, and actual read/write deadlines.
 */
export module mhttp:test.client;

import std;
import asio;
import :client.http_client;
import :test.transport;
import :test.client_peer;

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
            { "client response framing", client_response_framing },
            { "client truncated responses", client_truncated_responses },
            { "client response limits", client_response_limits },
            { "client content encoding", client_content_encoding },
            { "client phase deadlines", client_phase_deadlines },
            { "client connection refused", client_connection_refused },
            { "client partial head no retry", client_partial_head_no_retry },
        });
    }

} // namespace mhttp::test
