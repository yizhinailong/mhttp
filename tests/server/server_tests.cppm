/**
 * @file server_tests.cppm
 * @brief Verify public listeners using independent raw peers and multithreaded runtime generations.
 */
export module mhttp:test.server;

import std;
import asio;
import :core.impl.io_error;
import :server.http_server;
import :test.transport;
import :test.session;

namespace mhttp::test {

    /**
     * @brief Connect an owned peer to a dynamically bound numeric loopback endpoint.
     * @param executor Independent fixture strand.
     * @param endpoint Owned server endpoint.
     * @return Connected socket, owned through every fixture await.
     */
    auto server_peer(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<std::shared_ptr<asio::ip::tcp::socket>> {
        auto peer = std::make_shared<asio::ip::tcp::socket>(executor);
        co_await peer->async_connect({ asio::ip::make_address(endpoint.address), endpoint.port }, asio::use_awaitable);
        co_return peer;
    }

    /**
     * @brief Collect complete response bytes through FIN or an abrupt server stop.
     * @param peer Owned connected socket.
     * @return Owned bytes, including any complete response preceding stop.
     */
    auto receive_server(std::shared_ptr<asio::ip::tcp::socket> peer) -> asio::awaitable<std::string> {
        std::string wire;
        std::array<char, 257> buffer{};
        for (;;) {
            auto [error, count] = co_await peer->async_read_some(asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
            wire.append(buffer.data(), count);
            if (error == asio::error::eof || error == asio::error::connection_reset) { co_return wire; }
            if (error) { throw std::system_error{ error }; }
        }
    }

    /**
     * @brief Read one byte at a time to avoid consuming a future response before signaling idle state.
     * @param peer Owned raw socket.
     * @return One complete final response's owned bytes.
     */
    auto receive_one_server(std::shared_ptr<asio::ip::tcp::socket> peer) -> asio::awaitable<std::string> {
        detail::ResponseParser parser;
        std::string wire;
        char byte{};
        for (;;) {
            auto step = parser.Feed({});
            if (step.event == detail::ParseEvent::Complete) { co_return wire; }
            if (step.event == detail::ParseEvent::Failed) { throw std::runtime_error{ "invalid server response" }; }
            co_await asio::async_read(*peer, asio::buffer(&byte, 1), asio::use_awaitable);
            wire += byte;
            step = parser.Feed(std::span<char const>{ &byte, 1 });
            if (step.event == detail::ParseEvent::Failed) { throw std::runtime_error{ "invalid server response byte" }; }
        }
    }

    /**
     * @brief Send one owned request and await the server's closing response.
     * @param executor Fixture strand.
     * @param endpoint Owned server endpoint.
     * @param input Owned request(s).
     * @return Complete wire response(s).
     */
    auto server_exchange(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::string input
    ) -> asio::awaitable<std::string> {
        auto peer = co_await server_peer(executor, std::move(endpoint));
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        co_return co_await receive_server(std::move(peer));
    }

    /**
     * @brief Join one raw peer waiting for cancellation while asserting no response was generated.
     * @param executor Fixture strand.
     * @param endpoint Owned dynamic endpoint.
     */
    auto server_exchange_void(asio::any_io_executor executor, Endpoint endpoint) -> asio::awaitable<void> {
        Check((co_await server_exchange(executor, std::move(endpoint),
            "GET / HTTP/1.1\r\nHost: local\r\n\r\n")).empty(), "stopped Offload request writes no response");
    }

    /**
     * @brief Check raw pipeline replies from every common method and rejection after stop.
     * @param executor Fixture strand.
     * @param endpoint Owned dynamic endpoint.
     */
    auto server_methods_scenario(asio::any_io_executor executor, Endpoint endpoint) -> asio::awaitable<void> {
        std::string input;
        std::array const methods{ Method::Get, Method::Head, Method::Post, Method::Put,
            Method::Patch, Method::Delete, Method::Options, Method::Trace };
        for (auto method : methods) {
            input += std::format("{} /item/42?q=raw+query HTTP/1.1\r\nHost: local\r\nContent-Length: 3\r\n{}\r\nabc",
                ToString(method), method == methods.back() ? "Connection: close\r\n" : "");
        }
        auto wire = co_await server_exchange(executor, endpoint, std::move(input));
        std::size_t offset{};
        for (auto method : methods) {
            auto reply = parse_session_reply(wire, offset, method);
            CheckEq(reply.head.status, Status::Ok, "method succeeds through public listener");
            CheckEq(reply.body, method == Method::Head ? std::string{} : std::string{ ToString(method) },
                "method-specific content and HEAD suppression");
        }
        CheckEq(offset, wire.size(), "listener sends exactly one ordered response per request");
    }

    /**
     * @brief Verify rejection against an endpoint whose listener has completed stop.
     * @param executor Fixture strand.
     * @param endpoint Former bound endpoint.
     */
    auto server_refused_scenario(asio::any_io_executor executor, Endpoint endpoint) -> asio::awaitable<void> {
        asio::ip::tcp::socket peer{ executor };
        auto [error] = co_await peer.async_connect({ asio::ip::make_address(endpoint.address), endpoint.port },
            asio::as_tuple(asio::use_awaitable));
        Check(static_cast<bool>(error), "stopped listener refuses new connections");
    }

    /**
     * @brief Verify Runtime ownership reaches Offload and routing survives a complete listener restart.
     */
    auto server_methods_and_restart() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start listener runtime");
        server::HttpServer server{ runtime };
        std::atomic<int> handlers{};
        for (auto method : { Method::Get, Method::Head, Method::Post, Method::Put,
                 Method::Patch, Method::Delete, Method::Options, Method::Trace }) {
            Check(server.Route(method, "/item/:id", [&](auto& request, auto& response) -> Task<void> {
                Check(Runtime::IsIoThread(), "listener handler starts on I/O execution");
                CheckEq(request.template PathParam<int>("id"), Result<int>{ 42 }, "typed path capture reaches handler");
                CheckEq(request.Query().Get("q"), "raw query", "query and body coexist");
                CheckEq(request.Body(), "abc", "all methods receive complete buffered bodies");
                CheckEq(request.RemoteEndpoint().address, "127.0.0.1", "raw peer address snapshot");
                auto value = co_await Offload([method = std::string{ request.MethodString() }] {
                    Check(!Runtime::IsIoThread(), "Offload executes outside I/O threads");
                    return method;
                });
                Check(Runtime::IsIoThread(), "handler resumes on I/O execution after Offload");
                ++handlers;
                Check(response.SetContent(std::move(value), "text/plain").has_value(), "set method content");
            }).has_value(), "register common method");
        }
        TransportRuntime peers;
        for (unsigned iteration{}; iteration < 2; ++iteration) {
            Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind real listener");
            Check(server.Start().has_value(), "start real listener");
            Check(server.WaitUntilReady().has_value(), "real listener ready");
            auto endpoint = *server.LocalEndpoint();
            peers.Run(server_methods_scenario(peers.Executor(), endpoint));
            Check(server.Stop().has_value(), "drain real listener");
            peers.Run(server_refused_scenario(peers.Executor(), endpoint));
        }
        CheckEq(handlers.load(), 16, "restart retains all registered callbacks");
    }

    /**
     * @brief Signal an idle keep-alive socket after its first complete response, then await stop.
     * @param executor Fixture strand.
     * @param endpoint Owned server endpoint.
     * @param idle Cross-thread publication after the complete response arrives.
     */
    auto server_idle_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::latch& idle
    ) -> asio::awaitable<void> {
        auto peer = co_await server_peer(executor, std::move(endpoint));
        std::string const input = "GET / HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        auto wire = co_await receive_one_server(peer);
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "idle", "read first response before stopping idle peer");
        idle.count_down();
        Check((co_await receive_server(std::move(peer))).empty(), "idle stop emits no extra response");
    }

    /**
     * @brief Verify idle keep-alive and automatic destruction drain promptly instead of waiting for read expiry.
     */
    auto server_idle_and_destructor_stop() -> void {
        for (bool destroy : { false, true }) {
            Runtime runtime;
            Check(runtime.Start(4).has_value(), "start idle runtime");
            auto server = std::make_unique<server::HttpServer>(runtime);
            Check(server->Get("/", [](auto&, auto& response) {
                Check(response.SetContent("idle", "text/plain").has_value(), "idle response");
            }).has_value(), "register idle handler");
            Check(server->BindToAnyPort("127.0.0.1").has_value(), "bind idle listener");
            Check(server->Start().has_value(), "start idle listener");
            TransportRuntime peers;
            std::latch idle{ 1 };
            std::jthread client{ [&] {
                peers.Run(server_idle_scenario(peers.Executor(), *server->LocalEndpoint(), idle));
            } };
            idle.wait();
            auto const started = std::chrono::steady_clock::now();
            if (destroy) { server.reset(); }
            else { Check(server->Stop().has_value(), "stop an idle keep-alive session"); }
            Check(std::chrono::steady_clock::now() - started < 1s, "idle drain finishes within one second");
        }
    }

    /**
     * @brief Start several raw requests and await cancellation of all owning peer sockets.
     * @param executor Fixture strand.
     * @param endpoint Owned server endpoint.
     */
    auto server_waiting_scenario(asio::any_io_executor executor, Endpoint endpoint) -> asio::awaitable<void> {
        std::vector<std::shared_ptr<TransportResult<std::string>>> clients;
        for (unsigned index{}; index < 3; ++index) {
            clients.push_back(spawn_transport(executor, server_exchange(executor, endpoint,
                "GET / HTTP/1.1\r\nHost: local\r\n\r\n")));
        }
        for (auto const& client : clients) {
            Check((co_await client->Wait()).empty(), "canceled handler sends no 500 or partial reply");
        }
    }

    /**
     * @brief Count coroutine-frame destruction without relying on shared callback captures being retired.
     */
    struct ActiveHandler {
        std::atomic<int>& active; ///< Counter outliving every handler and root.
        /**
         * @brief Publish handler entry.
         * @param count Borrowed synchronized counter.
         */
        explicit ActiveHandler(std::atomic<int>& count) : active{ count } { ++active; }
        /**
         * @brief Publish request-frame cleanup before the listener reports completion.
         */
        ~ActiveHandler() { --active; }
    };

    /**
     * @brief Verify explicit server stop and Runtime stop both drain three suspended request frames.
     */
    auto server_inflight_stop() -> void {
        for (bool stop_runtime : { false, true }) {
            Runtime runtime;
            Check(runtime.Start(4).has_value(), "start in-flight runtime");
            server::HttpServer server{ runtime };
            std::latch entered{ 3 };
            std::atomic<int> active{};
            Check(server.Get("/", [&](auto&, auto&) -> Task<void> {
                ActiveHandler owner{ active };
                entered.count_down();
                co_await Sleep(24h);
                Check(false, "canceled handler cannot reach post-Sleep code");
            }).has_value(), "register in-flight handler");
            Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind in-flight listener");
            Check(server.Start().has_value(), "start in-flight listener");
            TransportRuntime peers;
            std::jthread client{ [&] { peers.Run(server_waiting_scenario(peers.Executor(), *server.LocalEndpoint())); } };
            entered.wait();
            if (stop_runtime) {
                runtime.Stop();
                Check(runtime.Join().has_value(), "Runtime stop drains accept and in-flight user Tasks");
            }
            Check(server.Stop().has_value(), "server stop drains all in-flight user Tasks");
            CheckEq(active.load(), 0, "stop completion follows every handler frame destruction");
            Check(server.Get("/after", [](auto&, auto&) {}).has_value(), "no readers remain after in-flight stop");
        }
    }

    /**
     * @brief Verify AsyncStop waits for already-running Offload work and keeps the request alive throughout.
     */
    auto server_offload_drain() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start offload drain runtime");
        server::HttpServer server{ runtime };
        std::latch entered{ 1 }, release{ 1 };
        std::atomic<int> active{};
        Check(server.Get("/", [&](auto& request, auto&) -> Task<void> {
            ActiveHandler owner{ active };
            co_await Offload([&] {
                entered.count_down();
                release.wait();
                CheckEq(request.Path(), "/", "request remains alive until blocked work drains");
            });
            Check(false, "canceled Offload cannot resume handler successfully");
        }).has_value(), "register blocking worker handler");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind offload drain listener");
        Check(server.Start().has_value(), "start offload drain listener");
        TransportRuntime peers;
        auto endpoint = *server.LocalEndpoint();
        std::jthread client{ [&] {
            peers.Run(server_exchange_void(peers.Executor(), endpoint));
        } };
        entered.wait();
        std::atomic<bool> drained{};
        std::jthread stopper{ [&] { SyncWait(runtime, server.AsyncStop()); drained = true; } };
        while (server.IsRunning()) { std::this_thread::sleep_for(1ms); }
        Check(!drained && active == 1, "AsyncStop remains pending while borrowed worker state is live");
        release.count_down();
        stopper.join();
        Check(drained && active == 0, "AsyncStop finishes after worker and handler cleanup");
        Check(server.Get("/after", [](auto&, auto&) {}).has_value(), "Offload drain releases route readers");
    }

    /**
     * @brief Verify failed callbacks affect one connection while the listener continues serving requests.
     * @param executor Fixture strand.
     * @param endpoint Owned dynamic endpoint.
     */
    auto server_survival_scenario(asio::any_io_executor executor, Endpoint endpoint) -> asio::awaitable<void> {
        for (auto path : { "/bad-post", "/standard", "/unknown", "/ok" }) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", path));
            if (path == std::string_view{ "/bad-post" }) {
                Check(wire.empty(), "throwing post closes only its connection without output");
                continue;
            }
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, path == std::string_view{ "/ok" } ? Status::Ok : Status::InternalServerError,
                "listener survives application and post-routing exceptions");
            Check(!wire.contains("secret failure"), "default exception responses contain no private exception message");
            CheckEq(reply.head.headers.Get("X-Server"), "default", "configured response defaults reach every final reply");
        }
    }

    /**
     * @brief Run exception isolation against a live listener and later successful request.
     */
    auto server_callback_isolation() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start exception isolation runtime");
        server::ServerOptions options;
        Check(options.default_headers.Set("X-Server", "default").has_value(), "configure server default field");
        server::HttpServer server{ runtime, options };
        Check(server.Get("/:name", [](auto& request, auto& response) {
            if (request.Path() == "/standard") { throw std::runtime_error{ "secret failure\r\nInjected: evil" }; }
            if (request.Path() == "/unknown") { throw 42; }
            Check(response.SetContent("healthy", "text/plain").has_value(), "healthy response after failures");
        }).has_value(), "register exception routes");
        Check(server.SetPostRoutingHandler([](auto& request, auto&) {
            if (request.Path() == "/bad-post") { throw std::runtime_error{ "post failure" }; }
        }).has_value(), "register isolated post failure");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind exception isolation listener");
        Check(server.Start().has_value(), "start exception isolation listener");
        TransportRuntime peers;
        peers.Run(server_survival_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.IsRunning(), "callback exceptions leave accept loop running");
        Check(server.Stop().has_value(), "drain exception isolation listener");
    }

    /**
     * @brief Verify a real request served by Listen, with Runtime execution on the caller when idle.
     * @param executor Fixture strand.
     * @param endpoint Owned dynamic endpoint.
     */
    auto server_listen_scenario(asio::any_io_executor executor, Endpoint endpoint) -> asio::awaitable<void> {
        auto wire = co_await server_exchange(executor, std::move(endpoint),
            "GET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "inline", "Listen handles a complete HTTP exchange");
    }

    /**
     * @brief Verify idle and started Runtime modes and the actual execution thread selected by Listen.
     */
    auto server_listen_execution() -> void {
        for (bool started_runtime : { false, true }) {
            Runtime runtime;
            if (started_runtime) { Check(runtime.Start(4).has_value(), "start shared Listen request runtime"); }
            server::HttpServer server{ runtime };
            std::thread::id listening_thread;
            Check(server.Get("/", [&](auto&, auto& response) -> Task<void> {
                Check(Runtime::IsIoThread(), "Listen handler runs in Runtime execution");
                if (!started_runtime) { CheckEq(std::this_thread::get_id(), listening_thread, "Listen runs on caller"); }
                co_await Sleep(1ms);
                if (!started_runtime) { CheckEq(std::this_thread::get_id(), listening_thread, "Task resumes on inline caller"); }
                Check(response.SetContent("inline", "text/plain").has_value(), "inline Listen response");
            }).has_value(), "register Listen request");
            std::optional<Result<void>> result;
            std::jthread listener{ [&] {
                listening_thread = std::this_thread::get_id();
                result = server.Listen("127.0.0.1", 0);
            } };
            while (!server.IsRunning()) { std::this_thread::sleep_for(1ms); }
            Check(server.WaitUntilReady().has_value(), "Listen request ready");
            TransportRuntime peers;
            peers.Run(server_listen_scenario(peers.Executor(), *server.LocalEndpoint()));
            Check(server.Stop().has_value(), "stop blocking Listen after request");
            listener.join();
            Check(result && result->has_value(), "Listen request loop finishes successfully");
            CheckEq(runtime.IsRunning(), started_runtime, "Listen preserves shared runtime ownership");
        }
    }

    /**
     * @brief Hold one keep-alive session, reject surplus peers, then observe capacity released after drain.
     * @param executor Fixture strand independent of the listener and session strands.
     * @param endpoint Owned dynamic loopback endpoint.
     */
    auto server_capacity_scenario(asio::any_io_executor executor, Endpoint endpoint) -> asio::awaitable<void> {
        auto first = co_await server_peer(executor, endpoint);
        std::string const persistent = "GET / HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*first, asio::buffer(persistent), asio::use_awaitable);
        auto reply = co_await receive_one_server(first);
        std::size_t offset{};
        CheckEq(parse_session_reply(reply, offset).body, "capacity", "first connection occupies the only slot");
        for (unsigned index{}; index < 3; ++index) {
            auto surplus = co_await server_peer(executor, endpoint);
            Check((co_await receive_server(std::move(surplus))).empty(), "surplus peer closed before request parsing");
        }
        co_await asio::async_write(*first, asio::buffer(persistent), asio::use_awaitable);
        reply = co_await receive_one_server(first);
        offset = 0;
        CheckEq(parse_session_reply(reply, offset).body, "capacity", "surplus rejection does not disrupt existing session");
        first->shutdown(asio::socket_base::shutdown_send);
        Check((co_await receive_server(std::move(first))).empty(), "idle session drains normally");

        // FIN can reach the peer before the server-strand completion erases the session registry entry.
        asio::steady_timer retry{ executor };
        auto const deadline = std::chrono::steady_clock::now() + 1s;
        bool admitted{};
        while (!admitted && std::chrono::steady_clock::now() < deadline) {
            auto peer = co_await server_peer(executor, endpoint);
            std::string const input = "GET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
            auto [error, count] = co_await asio::async_write(*peer, asio::buffer(input), asio::as_tuple(asio::use_awaitable));
            if (!error && count == input.size()) {
                auto wire = co_await receive_server(std::move(peer));
                if (!wire.empty()) {
                    offset = 0;
                    CheckEq(parse_session_reply(wire, offset).body, "capacity", "drained slot accepts a new session");
                    admitted = true;
                }
            }
            if (!admitted) { retry.expires_after(2ms); co_await retry.async_wait(asio::use_awaitable); }
        }
        Check(admitted, "capacity becomes available within the bounded drain deadline");
    }

    /**
     * @brief Verify the finite session admission bound across a listener restart.
     */
    auto server_connection_limit() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start capacity runtime");
        server::HttpServer server{ runtime, { .max_connections = 1 } };
        std::atomic<int> handlers{};
        Check(server.Get("/", [&](auto&, auto& response) {
            ++handlers;
            Check(response.SetContent("capacity", "text/plain").has_value(), "capacity response");
        }).has_value(), "register capacity route");
        TransportRuntime peers;
        for (unsigned iteration{}; iteration < 2; ++iteration) {
            Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind limited listener");
            Check(server.Start().has_value(), "start limited listener");
            peers.Run(server_capacity_scenario(peers.Executor(), *server.LocalEndpoint()));
            Check(server.Stop().has_value(), "drain limited listener");
        }
        CheckEq(handlers.load(), 6, "only admitted requests execute handlers");
    }

} // namespace mhttp::test

export namespace mhttp::test {

    /**
     * @brief Run public listener scenarios with bounded per-case watchdogs.
     * @param argc Argument count.
     * @param argv Optional case filters.
     * @return Process status.
     */
    auto RunServerTests(int argc, char* argv[]) -> int {
        return RunTests(argc, argv, {
            { "listener methods and restart", server_methods_and_restart },
            { "listener idle and destructor stop", server_idle_and_destructor_stop },
            { "listener in-flight stop", server_inflight_stop },
            { "listener Offload drain", server_offload_drain },
            { "listener callback isolation", server_callback_isolation },
            { "listener Listen execution", server_listen_execution },
            { "listener connection limit", server_connection_limit },
        });
    }

} // namespace mhttp::test
