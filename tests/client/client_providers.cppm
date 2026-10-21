/**
 * @file client_providers.cppm
 * @brief Verify provider retry ownership, transport backpressure, and Expect fallback against real peers.
 */
module mhttp:test.client_providers;

import std;
import asio;
import :client.http_client;
import :server.http_server;
import :test.client_peer;
import :test.client_write_fault;
import :test.transport;

namespace mhttp::test {

    /**
     * @brief Keep provider state untouched until a complete head, replaying only a zero-byte failure on a reused socket.
     */
    auto client_provider_head_failures() -> void {
        for (unsigned mode{}; mode != 3; ++mode) {
            Runtime runtime;
            Check(runtime.Start(4).has_value(), "start provider head retry runtime");
            server::HttpServer server{ runtime };
            std::atomic<unsigned> uploads{};
            Check(server.Get("/warmup", [](auto&, auto&) {}).has_value(), "provider retry warmup");
            Check(server.Post("/retry", [&](auto& request, auto& response) {
                ++uploads;
                CheckEq(request.Body(), "data", "provider upload is transmitted exactly once");
                static_cast<void>(response.SetContent("stored", "text/plain"));
            }).has_value(), "provider retry receiver");
            auto port = server.BindToAnyPort("127.0.0.1");
            Check(port && server.Start() && server.WaitUntilReady(), "start provider retry listener");
            client::HttpClient client{ runtime, "127.0.0.1", *port, { .upload_chunk_bytes = 2 } };
            Check(SyncWait(runtime, client.Get("/warmup")).has_value(), "establish provider retry connection");
            std::vector<ClientWriteFailure> failures{ { Error::ConnectionClosed, mode == 1 ? 1u : 0u } };
            if (mode == 2) { failures.push_back({ Error::ConnectionClosed, 0 }); }
            ClientWriteFault::Configure(std::move(failures));
            unsigned calls{};
            client::Request request{ Method::Post, "/retry" };
            Check(request.SetContentProvider(4, [&](auto offset, auto maximum) -> Task<Result<std::string>> {
                ++calls;
                co_return std::string{ "data" }.substr(static_cast<std::size_t>(offset), maximum);
            }).has_value(), "defer provider until successful head");
            auto result = SyncWait(runtime, client.Send(std::move(request)));
            if (mode == 0) { Check(result && result->Body() == "stored" && calls == 2 && uploads == 1, "zero-byte reused-head failure retries before consuming provider"); }
            else { Check(!result && result.error() == Error::ConnectionClosed && calls == 0 && uploads == 0, "partial head or exhausted retry never invokes provider"); }
            CheckEq(ClientWriteFault::Calls(), mode == 1 ? std::size_t{ 1 } : std::size_t{ 2 }, "provider follows the original head retry budget");
            Check(server.Stop().has_value(), "drain provider retry fixture");
        }
        ClientWriteFault::Configure({});
    }

    /**
     * @brief Stop pulling a large source when the raw peer leaves its receive window unread.
     */
    auto client_provider_backpressure() -> void {
        unsigned calls{};
        client::Request request{ Method::Post, "/large" };
        Check(request.SetContentProvider(16 * 1024 * 1024, [&](auto, auto maximum) -> Task<Result<std::string>> {
            ++calls;
            co_return std::string(maximum, 'x');
        }).has_value(), "large bounded source");
        auto result = run_client_peer({ .pause = 400ms, .read_body = false },
            { .write_timeout = 100ms, .max_request_bytes = 128 * 1024, .upload_chunk_bytes = 64 * 1024 }, std::move(request));
        Check(!result.response && result.response.error() == Error::WriteTimeout && !result.open, "unread receive window reaches upload deadline");
        Check(calls < 256 && result.elapsed < 350ms, "provider stops at transport backpressure instead of materializing the complete upload");
    }

    /**
     * @brief Invoke a provider after bounded fallback when a peer waits for the upload without sending 100.
     */
    auto client_provider_expect_fallback() -> void {
        unsigned calls{};
        client::Request request{ Method::Post, "/upload" };
        Check(request.SetContentProvider(8, [&](auto offset, auto maximum) -> Task<Result<std::string>> {
            ++calls;
            co_return std::string{ "provider" }.substr(static_cast<std::size_t>(offset), maximum);
        }).has_value(), "provider behind a silent Expect handshake");
        auto result = run_client_peer({ "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok" },
            { .expect_continue_threshold = 1, .expect_continue_timeout = 50ms, .upload_chunk_bytes = 4 }, std::move(request));
        Check(result.response && result.response->Body() == "ok" && calls == 2, "fallback uploads each provider chunk once without 100");
        Check(result.request.contains("Expect: 100-continue\r\n") && result.request.ends_with("provider") && result.elapsed >= 40ms,
            "silent peer observes negotiated head then complete bounded provider upload");
    }

    /**
     * @brief Reset an established upload only after one complete provider chunk has reached the peer.
     * @param listener Owned dynamic loopback acceptor.
     * @param reset Cross-thread publication allowing the provider to attempt its next write after reset.
     */
    auto reset_provider_upload(
        std::shared_ptr<asio::ip::tcp::acceptor> listener,
        std::atomic<bool>& reset
    ) -> asio::awaitable<void> {
        asio::ip::tcp::socket socket{ co_await asio::this_coro::executor };
        co_await listener->async_accept(socket, asio::use_awaitable);
        auto head = co_await read_client_head(socket);
        Check(head.contains("Content-Length: 12\r\n"), "reset peer sees authoritative provider size");
        std::string prefix(4, '\0');
        co_await asio::async_read(socket, asio::buffer(prefix), asio::use_awaitable);
        CheckEq(prefix, "xxxx", "reset follows one completed application segment");
        socket.set_option(asio::socket_base::linger{ true, 0 });
        socket.close();
        reset = true;
    }

    /**
     * @brief Stop source production after a real peer reset and release provider frames without replay.
     */
    auto client_provider_peer_reset() -> void {
        TransportRuntime peer_runtime;
        auto listener = std::make_shared<asio::ip::tcp::acceptor>(peer_runtime.Executor(),
            asio::ip::tcp::endpoint{ asio::ip::address_v4::loopback(), 0 });
        auto const port = listener->local_endpoint().port();
        std::atomic<bool> reset{};
        std::exception_ptr failure;
        std::jthread worker{ [&] {
            try { peer_runtime.Run(reset_provider_upload(listener, reset)); }
            catch (...) { failure = std::current_exception(); }
        } };
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start reset provider runtime");
        client::HttpClient client{ runtime, "127.0.0.1", port, { .write_timeout = 2s, .upload_chunk_bytes = 4 } };
        unsigned calls{}, active{};
        client::Request request{ Method::Post, "/upload" };
        Check(request.SetContentProvider(12, [&](auto offset, auto count) -> Task<Result<std::string>> {
            ++calls;
            ++active;
            /**
             * @brief Observe provider frame destruction before Send reports the socket failure.
             */
            struct FrameLease {
                unsigned& count; ///< Counter inspected after SyncWait publishes completion.
                /**
                 * @brief Release on success, cancellation, and exception.
                 */
                ~FrameLease() { --count; }
            } frame{ active };
            if (offset != 0) {
                while (!reset.load()) { co_await Sleep(1ms); }
                co_await Sleep(20ms);
            }
            co_return std::string(count, 'x');
        }).has_value(), "provider gated behind reset after its first chunk");
        auto result = SyncWait(runtime, client.Send(std::move(request)));
        worker.join();
        if (failure) { std::rethrow_exception(failure); }
        Check(!result && result.error() == Error::ConnectionClosed && !client.IsOpen(), "actual upload reset reports connection closure");
        Check(calls == 2 && active == 0, "failed write prevents further provider calls and drains all callback frames");
    }

} // namespace mhttp::test
