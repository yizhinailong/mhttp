/**
 * @file client_expect.cppm
 * @brief Verify upload negotiation against independent raw TCP peers with bounded loopback scripts.
 */
module mhttp:test.client_expect;

import std;
import asio;
import :client.http_client;
import :test.transport;
import :test.client_peer;

namespace mhttp::test {

    using namespace std::chrono_literals;

    /**
     * @brief Choose whether a raw peer permits, delays, rejects, or never answers an upload.
     */
    enum class ExpectMode { Continue, Fallback, Reject, Quiet };

    /**
     * @brief Own the peer's wire script and independent expected request payload.
     */
    struct ExpectPlan {
        ExpectMode mode; ///< First response behavior.
        std::string body{ std::string(8192, 'x') }; ///< Independent expected binary upload bytes.
        std::string reply; ///< Early final or malformed response bytes.
        bool next{}; ///< Verify a second request on the correct existing/new TCP connection.
        bool half_close{}; ///< Delimit the first reply by peer EOF.
    };

    /**
     * @brief Retain peer observations only after joining its worker.
     */
    struct ExpectObserved {
        std::string head; ///< Independently consumed outgoing head.
        std::string body; ///< Bytes observed only after permitting an upload.
        unsigned connections{}; ///< Accepted TCP connections.
    };

    /**
     * @brief Pause a raw script without blocking either I/O runtime.
     * @param delay Bounded script gap.
     */
    auto expect_pause(std::chrono::milliseconds delay) -> asio::awaitable<void> {
        asio::steady_timer timer{ co_await asio::this_coro::executor };
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);
    }

    /**
     * @brief Observe exact body admission and socket closure without sharing any HTTP parser with the client.
     * @param listener Owned dynamic loopback acceptor.
     * @param plan Owned wire script.
     * @param observed Borrowed until the coordinator joins this coroutine.
     * @param ready Signal a quiet handshake for foreign Stop or Runtime cancellation.
     */
    auto serve_expect(
        std::shared_ptr<asio::ip::tcp::acceptor> listener,
        ExpectPlan plan,
        ExpectObserved& observed,
        std::latch& ready
    ) -> asio::awaitable<void> {
        auto executor = co_await asio::this_coro::executor;
        asio::ip::tcp::socket socket{ executor };
        co_await listener->async_accept(socket, asio::use_awaitable);
        socket.set_option(asio::ip::tcp::no_delay{ true });
        ++observed.connections;
        observed.head = co_await read_client_head(socket);
        Check(observed.head.starts_with("POST /upload HTTP/1.1\r\n"), "raw peer sees the intended upload method and target");
        Check(observed.head.contains("100-continue\r\n") || observed.head.contains("100-CoNtInUe\r\n"),
            "raw peer sees an explicit or automatically inserted expectation");
        Check(observed.head.contains(std::format("Content-Length: {}\r\n", plan.body.size())), "head promises the complete owned body");
        ready.count_down();
        bool const upload = plan.mode == ExpectMode::Continue || plan.mode == ExpectMode::Fallback;
        if (plan.mode == ExpectMode::Continue) {
            co_await expect_pause(50ms);
            CheckEq(socket.available(), std::size_t{}, "no upload bytes precede any response");
            std::string const hints{ "HTTP/1.1 103 Early Hints\r\nLink: </asset>\r\n\r\n" };
            co_await asio::async_write(socket, asio::buffer(hints), asio::use_awaitable);
            co_await expect_pause(50ms);
            CheckEq(socket.available(), std::size_t{}, "103 does not grant permission to upload");
            std::string const partial{ "HTTP/1.1 100 Continue\r\n" };
            co_await asio::async_write(socket, asio::buffer(partial), asio::use_awaitable);
            co_await expect_pause(50ms);
            CheckEq(socket.available(), std::size_t{}, "an incomplete 100 head cannot release the body");
            co_await asio::async_write(socket, asio::buffer("\r\n", 2), asio::use_awaitable);
        }
        if (upload) {
            auto const started = std::chrono::steady_clock::now();
            observed.body.resize(plan.body.size());
            co_await asio::async_read(socket, asio::buffer(observed.body), asio::use_awaitable);
            CheckEq(observed.body, plan.body, "upload preserves every owned binary byte");
            if (plan.mode == ExpectMode::Fallback) {
                auto const elapsed = std::chrono::steady_clock::now() - started;
                Check(elapsed >= 20ms && elapsed < 500ms, "quiet peer receives the body after the configured bounded wait");
                std::string const late{ "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n" };
                co_await asio::async_write(socket, asio::buffer(late), asio::use_awaitable);
            }
            std::string const reply{ "HTTP/1.1 201 Created\r\nContent-Length: 2\r\n\r\nok" };
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
        } else {
            if (!plan.reply.empty()) { co_await asio::async_write(socket, asio::buffer(plan.reply), asio::use_awaitable); }
            if (plan.half_close) { socket.shutdown(asio::socket_base::shutdown_send); }
            char unexpected{};
            auto const [error, count] = co_await socket.async_read_some(asio::buffer(&unexpected, 1), asio::as_tuple(asio::use_awaitable));
            Check(count == 0 && error == asio::error::eof, "rejected, expired, or canceled handshake closes without uploading any body");
        }
        if (plan.next) {
            if (!upload) {
                socket.close();
                co_await listener->async_accept(socket, asio::use_awaitable);
                ++observed.connections;
            }
            auto const head = co_await read_client_head(socket);
            Check(head.starts_with("GET /again HTTP/1.1\r\n"), "next head follows exactly one upload without late-100 duplication");
            Check(!head.contains("Expect:"), "empty successor does not inherit automatic Expect");
            std::string const reply{ "HTTP/1.1 200 OK\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnext" };
            co_await asio::async_write(socket, asio::buffer(reply), asio::use_awaitable);
        }
    }

    /**
     * @brief Return completed public results with peer-owned observations and first-response connection state.
     */
    struct ExpectResult {
        Result<client::Response> response; ///< Owned first response or failure.
        ExpectObserved observed; ///< Joined independent peer observations.
        bool open{}; ///< Published connection state after the first Send.
        std::chrono::steady_clock::duration elapsed; ///< First Send duration.
    };

    /**
     * @brief Drive public client Tasks while the raw peer runs on a separate runtime.
     * @param plan Owned raw protocol script.
     * @param options Client handshake and request deadlines.
     * @param stop Zero for no foreign cancellation, one for Client.Stop, two for Runtime.Stop.
     * @return Owned observations after every peer and stopper thread has joined.
     */
    auto run_expect(
        ExpectPlan plan,
        client::ClientOptions options,
        unsigned stop = 0
    ) -> ExpectResult {
        TransportRuntime peers;
        auto listener = std::make_shared<asio::ip::tcp::acceptor>(peers.Executor(),
            asio::ip::tcp::endpoint{ asio::ip::address_v4::loopback(), 0 });
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start Expect client runtime");
        client::HttpClient client{ runtime, "127.0.0.1", listener->local_endpoint().port(), options };
        client::Request request{ Method::Post, "/upload" };
        Check(request.SetContent(plan.body).has_value(), "own the complete pending upload");
        if (options.expect_continue_threshold == 0) {
            Check(request.Headers().Set("Expect", "100-CoNtInUe").has_value(), "explicit case-insensitive expectation");
        }
        bool const next = plan.next;
        ExpectObserved observed;
        std::exception_ptr failure;
        std::latch ready{ 1 };
        std::jthread worker{ [&peers, listener, plan = std::move(plan), &observed, &failure, &ready]() mutable {
            try { peers.Run(serve_expect(listener, std::move(plan), observed, ready)); }
            catch (...) { failure = std::current_exception(); }
            if (!ready.try_wait()) { ready.count_down(); }
        } };
        std::optional<std::jthread> stopper;
        if (stop != 0) {
            stopper.emplace([&] {
                ready.wait();
                if (stop == 1) { client.Stop(); }
                else { runtime.Stop(); }
            });
        }
        auto const started = std::chrono::steady_clock::now();
        auto response = SyncWait(runtime, client.Send(std::move(request)));
        auto const elapsed = std::chrono::steady_clock::now() - started;
        bool const open = client.IsOpen();
        if (next) {
            auto second = SyncWait(runtime, client.Get("/again"));
            Check(second && second->Body() == "next" && !second->KeepAlive(), "successor request completes on the expected socket");
        }
        if (stopper) { stopper->join(); }
        worker.join();
        if (failure) { std::rethrow_exception(failure); }
        return { std::move(response), std::move(observed), open, elapsed };
    }

    /**
     * @brief Wait for a complete 100 after 103, then preserve keep-alive without charging handshake time to writing.
     */
    auto client_expect_handshake() -> void {
        ExpectPlan plan{ .mode = ExpectMode::Continue, .next = true };
        plan.body[17] = '\0';
        auto result = run_expect(std::move(plan), { .read_timeout = 2s, .write_timeout = 100ms,
            .expect_continue_timeout = 1s });
        Check(result.response && result.response->Status() == Status::Created && result.response->Body() == "ok",
            "complete 100 releases the body before parsing the final response");
        Check(result.response && result.open && result.response->KeepAlive() && result.observed.connections == 1,
            "accepted upload keeps one connection reusable");
    }

    /**
     * @brief Fall back for a quiet peer even under Never, and ignore late repeated 100 without resending body bytes.
     */
    auto client_expect_fallback() -> void {
        for (auto policy : { client::TimeoutPolicy::Phase, client::TimeoutPolicy::Never }) {
            auto result = run_expect({ .mode = ExpectMode::Fallback, .next = true },
                { .read_timeout = 2s, .timeout_policy = policy, .expect_continue_threshold = 4096,
                    .expect_continue_timeout = 80ms });
            Check(result.response && result.response->Status() == Status::Created && result.response->Body() == "ok",
                "bounded fallback uploads and parses final bytes after late 100");
            Check(result.open && result.observed.connections == 1, "fallback preserves every response byte and the existing socket");
        }
    }

    /**
     * @brief Suppress uploads for complete early finals and close even when a peer advertises persistence.
     */
    auto client_expect_early_final() -> void {
        for (auto const& [wire, status] : std::to_array<std::pair<std::string_view, Status>>({
            { "HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 401 Unauthorized\r\nContent-Length: 3\r\n\r\nno!", Status::Unauthorized },
            { "HTTP/1.1 417 Expectation Failed\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nno!\r\n0\r\n\r\n", Status::ExpectationFailed },
            { "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\nno!", Status::Ok },
        })) {
            auto result = run_expect({ .mode = ExpectMode::Reject, .body = std::string(2 * 1024 * 1024, 'b'),
                .reply = std::string{ wire }, .next = true }, { .expect_continue_threshold = 1024 });
            Check(result.response && result.response->Status() == status && result.response->Body() == "no!",
                "early final remains a complete ordinary HTTP response");
            Check(result.response && !result.open && !result.response->KeepAlive() && result.observed.connections == 2,
                "early final prevents reuse and the next request establishes a fresh connection");
        }
        auto eof = run_expect({ .mode = ExpectMode::Reject, .reply = "HTTP/1.1 413 Content Too Large\r\n\r\nno!",
            .half_close = true }, {});
        Check(eof.response && eof.response->Status() == Status::ContentTooLarge && eof.response->Body() == "no!" && !eof.open,
            "early close-delimited final completes at EOF without an upload");
    }

    /**
     * @brief Let request read/overall/step deadlines win before the later handshake fallback.
     */
    auto client_expect_deadlines() -> void {
        for (auto policy : { client::TimeoutPolicy::Phase, client::TimeoutPolicy::Overall, client::TimeoutPolicy::Step }) {
            auto result = run_expect({ .mode = ExpectMode::Quiet }, { .read_timeout = 50ms,
                .timeout_policy = policy, .timeout = 50ms, .expect_continue_timeout = 1s });
            Check(!result.response && result.response.error() == Error::ReadTimeout && !result.open,
                "earlier request deadline terminates the handshake without sending its body");
            Check(result.elapsed >= 25ms && result.elapsed < 500ms, "handshake respects the earlier bounded request budget");
        }
    }

    /**
     * @brief Cancel untimed handshake waits from another thread and drain the peer before returning.
     */
    auto client_expect_cancellation() -> void {
        for (unsigned stop : { 1U, 2U }) {
            auto result = run_expect({ .mode = ExpectMode::Quiet }, { .timeout_policy = client::TimeoutPolicy::Never,
                .expect_continue_timeout = 5s }, stop);
            Check(!result.response && result.response.error() == Error::Canceled && !result.open,
                "Client.Stop and Runtime.Stop cancel an untimed pending upload without body bytes");
        }
    }

    /**
     * @brief Reject malformed/excessive intermediate heads before uploading anything.
     */
    auto client_expect_invalid_interims() -> void {
        std::string flood;
        for (unsigned count{}; count < 9; ++count) { flood += "HTTP/1.1 103 Early Hints\r\n\r\n"; }
        for (auto const& wire : { std::string{ "HTTP/1.1 100 Continue\r\nBad Field: x\r\n\r\n" },
                                 std::string{ "HTTP/1.1 101 Switching Protocols\r\n\r\n" }, flood }) {
            auto result = run_expect({ .mode = ExpectMode::Reject, .reply = wire }, {});
            Check(!result.response && result.response.error() == Error::ProtocolError && !result.open,
                "bad informational framing and excess 1xx close without body admission");
        }
        auto eof = run_expect({ .mode = ExpectMode::Reject, .half_close = true }, {});
        Check(!eof.response && eof.response.error() == Error::ConnectionClosed && !eof.open,
            "EOF before any 100 or final response fails without uploading or replaying");
        auto large = run_expect({ .mode = ExpectMode::Reject,
            .reply = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n" }, { .body_bytes = 4 });
        Check(!large.response && large.response.error() == Error::BodyTooLarge && !large.open,
            "early final response limits still apply before body admission");
    }

    /**
     * @brief Bound a stalled upload after permission and preserve the terminal write failure without replay.
     */
    auto client_expect_write_deadline() -> void {
        client::Request request{ Method::Post, "/upload" };
        Check(request.SetContent(std::string(8 * 1024 * 1024, 'b')).has_value(), "own a large pending upload");
        Check(request.Headers().Set("Expect", "100-continue").has_value(), "request permission before the blocked write");
        auto result = run_client_peer({ "HTTP/1.1 100 Continue\r\n\r\n", {}, 350ms, false },
            { .write_timeout = 100ms }, std::move(request));
        Check(!result.response && result.response.error() == Error::WriteTimeout && !result.open,
            "body write retains its deadline after a successful handshake");
        Check(result.elapsed >= 50ms && result.elapsed < 300ms, "stalled upload finishes before the nonreading peer closes");
    }

} // namespace mhttp::test
