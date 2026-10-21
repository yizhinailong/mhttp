/**
 * @file server_concurrency.cppm
 * @brief Verify concurrent persistent pipelines and isolate blocking Offload work from other connections.
 */
module mhttp:test.server_concurrency;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Pipeline twenty requests on one connection and verify identity, order, and final count-bound close.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback endpoint.
     * @param client Unique client identity.
     * @return Number of verified responses.
     */
    auto server_pipeline_client(
        asio::any_io_executor executor,
        Endpoint endpoint,
        unsigned client
    ) -> asio::awaitable<unsigned> {
        std::string input;
        for (unsigned index{}; index < 20; ++index) {
            input += std::format("GET /identity/{}/{} HTTP/1.1\r\nHost: local\r\n\r\n", client, index);
        }
        auto wire = co_await server_exchange(executor, std::move(endpoint), std::move(input));
        std::size_t offset{};
        for (unsigned index{}; index < 20; ++index) {
            auto reply = parse_session_reply(wire, offset);
            auto const expected = std::format("{}:{}", client, index);
            CheckEq(reply.head.status, Status::Ok, "concurrent pipeline status");
            CheckEq(reply.body, expected, "request identity and response order remain local to each connection");
            CheckEq(reply.head.headers.Get("Content-Length"), std::to_string(expected.size()), "concurrent response framing");
            CheckEq(reply.head.headers.Get("Connection") == "close", index == 19, "only response twenty closes its connection");
        }
        CheckEq(offset, wire.size(), "exactly twenty complete pipeline responses");
        co_return 20;
    }

    /**
     * @brief Run fifty independent persistent connections on the same multithreaded listener.
     * @param executor Fixture strand.
     * @param endpoint Owned loopback endpoint.
     */
    auto server_pipeline_clients(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        std::vector<std::shared_ptr<TransportResult<unsigned>>> clients;
        for (unsigned index{}; index < 50; ++index) {
            clients.push_back(spawn_transport(executor, server_pipeline_client(executor, endpoint, index)));
        }
        unsigned responses{};
        for (auto const& client : clients) { responses += co_await client->Wait(); }
        CheckEq(responses, 1000U, "fifty clients complete twenty requests each");
    }

    /**
     * @brief Verify connection-local request storage and ordered replies with fifty simultaneously active sessions.
     */
    auto server_concurrent_pipelines() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start concurrent pipeline runtime");
        std::atomic<unsigned> ready{}, calls{};
        server::HttpServer server{ runtime, { .keep_alive_max_requests = 20, .max_connections = 64 } };
        Check(server.Get("/identity/:client/:index", [&](auto& request, auto& response) -> Task<void> {
            auto const client = *request.template PathParam<unsigned>("client");
            auto const index = *request.template PathParam<unsigned>("index");
            if (index == 0) {
                ++ready;
                while (ready < 50) { co_await Sleep(1ms); }
            }
            co_await Sleep(1ms);
            CheckEq(request.template PathParam<unsigned>("client"), std::optional{ client }, "capture survives concurrent suspension");
            CheckEq(request.template PathParam<unsigned>("index"), std::optional{ index }, "pipeline successor cannot replace active request");
            ++calls;
            Check(response.SetContent(std::format("{}:{}", client, index), "text/plain").has_value(), "concurrent identity response");
        }).has_value(), "register concurrently suspended identity route");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind concurrent pipeline listener");
        Check(server.Start().has_value(), "start concurrent pipeline listener");
        TransportRuntime peers;
        peers.Run(server_pipeline_clients(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain concurrent pipeline sessions");
        Check(ready == 50 && calls == 1000, "all fifty sessions overlap and dispatch exactly one handler per request");
        Check(server.Get("/after", [](auto&, auto&) {}).has_value(), "concurrent session drain releases every route reader");
    }

    /**
     * @brief Request one complete response while retaining peer ownership through collection.
     * @param executor Fixture strand.
     * @param endpoint Owned loopback endpoint.
     * @param path Owned route path.
     * @param expected Owned expected response body.
     */
    auto server_offload_reply(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::string path,
        std::string expected
    ) -> asio::awaitable<void> {
        auto wire = co_await server_exchange(executor, std::move(endpoint),
            std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", path));
        std::size_t offset{};
        auto reply = parse_session_reply(wire, offset);
        CheckEq(reply.head.status, Status::Ok, "Offload isolation request status");
        CheckEq(reply.body, expected, "Offload isolation response identity");
        CheckEq(offset, wire.size(), "one complete Offload isolation response");
    }

    /**
     * @brief Verify half a second of blocking worker activity cannot delay another connection's HTTP response.
     */
    auto server_offload_isolation() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start Offload isolation runtime");
        std::latch entered{ 1 };
        std::atomic<bool> worker_done{};
        server::HttpServer server{ runtime };
        Check(server.Get("/slow", [&](auto&, auto& response) -> Task<void> {
            auto result = co_await Offload([&] {
                Check(!Runtime::IsIoThread(), "blocking work runs outside every I/O execution thread");
                entered.count_down();
                std::this_thread::sleep_for(500ms);
                worker_done = true;
                return std::string{ "slow" };
            });
            Check(Runtime::IsIoThread(), "handler resumes on I/O execution after worker completion");
            Check(response.SetContent(std::move(result), "text/plain").has_value(), "completed Offload response");
        }).has_value(), "register blocking worker route");
        Check(server.Get("/fast", [&](auto&, auto& response) {
            Check(!worker_done, "fast handler runs while the other connection is blocked in its worker");
            Check(response.SetContent("fast", "text/plain").has_value(), "unrelated fast response");
        }).has_value(), "register unrelated fast route");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind Offload isolation listener");
        Check(server.Start().has_value(), "start Offload isolation listener");
        TransportRuntime peers;
        auto const endpoint = *server.LocalEndpoint();
        std::jthread slow{ [&] { peers.Run(server_offload_reply(peers.Executor(), endpoint, "/slow", "slow")); } };
        entered.wait();
        auto const started = std::chrono::steady_clock::now();
        peers.Run(server_offload_reply(peers.Executor(), endpoint, "/fast", "fast"));
        Check(std::chrono::steady_clock::now() - started < 250ms, "fast response finishes well before the 500ms worker");
        Check(!worker_done, "fast response reaches the peer while worker remains active");
        slow.join();
        Check(worker_done.load(), "slow worker completes before test-owned state is released");
        Check(server.Stop().has_value(), "drain isolated worker and both sessions");
    }

} // namespace mhttp::test
