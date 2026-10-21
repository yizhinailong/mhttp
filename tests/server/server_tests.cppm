/**
 * @file server_tests.cppm
 * @brief Verify public listeners using independent raw peers and multithreaded runtime generations.
 */
export module mhttp:test.server;

import std;
import asio;
import :core.impl.io_error;
import :server.http_server;
import :server.impl.socket_options;
import :test.transport;
import :test.session;
import :test.server_peer;
import :test.server_routes;
import :test.server_handlers;
import :test.server_messages;
import :test.server_framing;
import :test.server_concurrency;
import :test.server_deadlines;

namespace mhttp::test {

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
     * @brief Verify stopping a bound-only socket releases the port without waiting for an accept generation.
     */
    auto server_bound_only_stop() -> void {
        for (bool started_runtime : { false, true }) {
            Runtime runtime;
            if (started_runtime) { Check(runtime.Start(4).has_value(), "start optional bound-only runtime"); }
            server::HttpServer server{ runtime };
            Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind without submitting an accept loop");
            auto const endpoint = *server.LocalEndpoint();
            CheckEq(server.WaitUntilReady().error(), Error::NotRunning, "bound-only readiness returns without waiting");
            Check(server.Stop().has_value(), "stop bound-only listener synchronously");
            CheckEq(server.WaitUntilReady().error(), Error::NotRunning, "stopped readiness returns without waiting");
            Check(!server.IsRunning() && !server.LocalEndpoint(), "bound-only stop clears published state");
            TransportRuntime peers;
            peers.Run(server_refused_scenario(peers.Executor(), endpoint));
            Check(server.Stop().has_value(), "repeated bound-only stop remains safe");
            CheckEq(runtime.IsRunning(), started_runtime, "bound-only stop preserves caller-owned runtime execution");
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
        std::atomic<int> diagnostics{}, accesses{};
        Check(server.SetLogger([&](auto const&) { ++diagnostics; }).has_value(), "observe offload cancellation");
        Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(),
            "observe canceled response writes");
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
        CheckEq(diagnostics.load(), 0, "explicit cancellation is not a session diagnostic");
        CheckEq(accesses.load(), 0, "canceled handler never completes a response for access observation");
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
            Check(!reply.head.headers.Has("Injected"), "default exception handling rejects CRLF header injection");
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
        std::atomic<int> rejected{};
        Check(server.SetLogger([&](auto const& record) {
            Check(Runtime::IsIoThread(), "rejection diagnostics execute on the server strand");
            CheckEq(record.event, server::LogEvent::ConnectionRejected, "only surplus sockets produce diagnostics");
            CheckEq(record.error, Error::Busy, "capacity diagnostic reason");
            Check(record.remote && record.remote->port != 0 && record.local && record.local->port != 0,
                "rejection includes both established endpoints");
            Check(!record.native_error && !record.exception, "capacity rejection has no native or callback failure");
            ++rejected;
        }).has_value(), "observe capacity rejections");
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
        Check(rejected.load() >= 6, "all deliberate surplus peers are diagnosed across restart");
    }

    /**
     * @brief Check actual listener option values and protection against competing reuse-enabled binds.
     */
    auto server_listener_socket_options() -> void {
        asio::io_context context;
        for (bool reuse : { false, true }) {
            asio::ip::tcp::acceptor acceptor{ context };
            acceptor.open(asio::ip::tcp::v4());
            Check(!impl::configure_listener_socket(acceptor, asio::ip::tcp::v4(), { .reuse_address = reuse }),
                "configure initialized IPv4 acceptor");
            asio::socket_base::reuse_address actual;
            acceptor.get_option(actual);
#ifdef _WIN32
            Check(!actual.value(), "Windows never enables SO_REUSEADDR");
            impl::ExclusiveAddressUse exclusive;
            acceptor.get_option(exclusive);
            Check(exclusive.value(), "Windows explicitly reserves an exclusive address");
#else
            CheckEq(actual.value(), reuse, "POSIX applies the configured reuse policy");
#endif
            acceptor.bind({ asio::ip::address_v4::loopback(), 0 });
            acceptor.listen();
            asio::ip::tcp::acceptor competitor{ context };
            competitor.open(asio::ip::tcp::v4());
            competitor.set_option(asio::socket_base::reuse_address{ true });
            asio::error_code error;
            competitor.bind(acceptor.local_endpoint(), error);
            Check(static_cast<bool>(error), "reuse-enabled competitor cannot take a live listener's address");
        }
        asio::ip::tcp::acceptor probe{ context };
        asio::error_code error;
        probe.open(asio::ip::tcp::v6(), error);
        if (error) { std::println("[skip] local IPv6 socket support unavailable: {}", error.message()); return; }
        probe.close();
        for (bool ipv6_only : { false, true }) {
            asio::ip::tcp::acceptor acceptor{ context };
            acceptor.open(asio::ip::tcp::v6());
            Check(!impl::configure_listener_socket(acceptor, asio::ip::tcp::v6(), { .ipv6_only = ipv6_only }),
                "configure explicit IPv6 mode");
            impl::IPv6Only actual{ false };
            acceptor.get_option(actual);
            CheckEq(actual.value(), ipv6_only, "native IPV6_V6ONLY matches configuration");
        }
    }

    /**
     * @brief Check actual TCP_NODELAY values before handing a connected socket to a session.
     * @param executor Independent fixture strand.
     */
    auto server_peer_options_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        for (bool no_delay : { false, true }) {
            auto& socket = pair->connection->SocketForInitialization();
            Check(!impl::configure_session_socket(socket, { .tcp_no_delay = no_delay }), "configure TCP_NODELAY");
            asio::ip::tcp::no_delay actual;
            socket.get_option(actual);
            CheckEq(actual.value(), no_delay, "native TCP_NODELAY matches configuration");
        }
        pair->connection->Close();
    }

    /**
     * @brief Run accepted-socket initialization on a real loopback pair.
     */
    auto server_peer_socket_options() -> void {
        TransportRuntime peers;
        peers.Run(server_peer_options_scenario(peers.Executor()));
    }

    /**
     * @brief Request the same IPv6 listener through native IPv6 and optionally mapped IPv4.
     * @param executor Fixture strand.
     * @param port Bound dynamic listener port.
     * @param dual_stack Whether to verify IPv4 requests in addition to IPv6.
     */
    auto server_ipv6_scenario(
        asio::any_io_executor executor,
        std::uint16_t port,
        bool dual_stack
    ) -> asio::awaitable<void> {
        for (auto host : { "::1", "127.0.0.1" }) {
            if (!dual_stack && host == std::string_view{ "127.0.0.1" }) { continue; }
            auto wire = co_await server_exchange(executor, { host, port },
                "GET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
            std::size_t offset{};
            CheckEq(parse_session_reply(wire, offset).body, "IPv6", "IPv6 listener serves the requested loopback family");
        }
    }

    /**
     * @brief Verify dual-stack and IPv6-only listeners, skipping only unavailable local IPv6 support.
     */
    auto server_ipv6_and_dual_stack() -> void {
        asio::io_context probe_context;
        asio::ip::tcp::acceptor probe{ probe_context };
        asio::error_code error;
        probe.open(asio::ip::tcp::v6(), error);
        if (!error) { probe.bind({ asio::ip::address_v6::loopback(), 0 }, error); }
        if (error) { std::println("[skip] local IPv6 loopback unavailable: {}", error.message()); return; }
        probe.close();
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start IPv6 runtime");
        TransportRuntime peers;
        for (bool ipv6_only : { false, true }) {
            server::HttpServer server{ runtime, { .ipv6_only = ipv6_only } };
            Check(server.Get("/", [](auto&, auto& response) {
                Check(response.SetContent("IPv6", "text/plain").has_value(), "IPv6 response");
            }).has_value(), "register IPv6 route");
            auto port = server.BindToAnyPort("::");
            if (!Check(port.has_value(), "bind IPv6 wildcard listener")) { continue; }
            Check(server.Start().has_value(), "start IPv6 listener");
            peers.Run(server_ipv6_scenario(peers.Executor(), *port, !ipv6_only));
            Check(server.Stop().has_value(), "drain IPv6 listener");
        }
    }

    /**
     * @brief Retain only owned access-log metadata after request and response references expire.
     */
    struct AccessEntry {
        std::string method; ///< Owned method token.
        std::string path; ///< Owned application path, sanitized for invalid heads.
        Status status; ///< Final application status, including serialization fallback.
        std::string body; ///< Final representation, including HEAD's suppressed body.
        std::chrono::steady_clock::duration elapsed; ///< Complete request duration excluding idle wait.
    };

    /**
     * @brief Exercise access records for normal, HEAD, interim, error, fallback, and rejected responses.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback endpoint.
     */
    auto server_access_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        auto peer = co_await server_peer(executor, endpoint);
        std::string const first = "GET /time HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(first), asio::use_awaitable);
        static_cast<void>(co_await receive_one_server(peer));
        asio::steady_timer idle{ executor, 250ms };
        co_await idle.async_wait(asio::use_awaitable);
        std::string const second = "GET /time HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(second), asio::use_awaitable);
        static_cast<void>(co_await receive_server(std::move(peer)));
        for (auto input : {
            "HEAD /time HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n",
            "POST /echo HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc",
            "GET /missing HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n",
            "GET /fail HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n",
            "GET /large HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n",
            "GET /post-fail HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n",
            "GET / HTTP/1.2\r\nHost: local\r\n\r\n",
        }) { static_cast<void>(co_await server_exchange(executor, endpoint, input)); }
    }

    /**
     * @brief Verify access callback ownership, finalized data, one-call semantics, and duration boundaries.
     */
    auto server_access_observation() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start access observer runtime");
        server::HttpServer server{ runtime, { .max_response_bytes = 512 } };
        Check(server.Get("/time", [](auto&, auto& response) -> Task<void> {
            co_await Sleep(5ms);
            Check(response.SetContent("timed", "text/plain").has_value(), "timed response");
        }).has_value(), "register timed route");
        Check(server.Post("/echo", [](auto& request, auto& response) {
            Check(response.SetContent(request.Body(), "text/plain").has_value(), "logged Expect response");
        }).has_value(), "register logged upload");
        Check(server.Get("/fail", [](auto&, auto&) { throw 42; }).has_value(), "register logged exception");
        Check(server.Get("/large", [](auto&, auto& response) {
            Check(response.SetContent(std::string(2048, 'x'), "text/plain").has_value(), "oversized application response");
        }).has_value(), "register fallback response");
        Check(server.SetPostRoutingHandler([](auto& request, auto& response) {
            if (request.Path() == "/post-fail") { throw 42; }
            Check(response.Headers().Set("X-Post", "complete").has_value(), "mark finalized response");
        }).has_value(), "register observer post sentinel");
        std::mutex mutex;
        std::vector<AccessEntry> entries;
        Check(server.SetAccessLogger([owner = std::make_unique<int>(7), &server, &mutex, &entries](
            auto const& request, auto const& response, auto elapsed) {
            Check(Runtime::IsIoThread(), "access callback executes on session I/O execution");
            CheckEq(*owner, 7, "move-only access capture survives every request");
            CheckEq(server.SetAccessLogger({}).error(), Error::AlreadyRunning, "active observer cannot replace itself");
            Check(request.RemoteEndpoint().port != 0 && request.LocalEndpoint().port != 0, "observer sees owned endpoints");
            if (request.Path() != "/large") { CheckEq(response.Header("X-Post"), "complete", "post runs before access observation"); }
            std::lock_guard lock{ mutex };
            entries.push_back({ std::string{ request.MethodString() }, std::string{ request.Path() },
                response.Status(), response.Body(), elapsed });
        }).has_value(), "configure owned access observer");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind observed listener");
        Check(server.Start().has_value(), "start observed listener");
        TransportRuntime peers;
        peers.Run(server_access_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain all access callbacks before inspection");
        CheckEq(entries.size(), std::size_t{ 8 }, "one access per final response, none for 100 or failed post");
        std::size_t timed{};
        for (auto const& entry : entries) {
            if (entry.path == "/time") {
                ++timed;
                Check(entry.elapsed >= 3ms && entry.elapsed < 200ms, "access duration includes handler and excludes 250ms idle wait");
                CheckEq(entry.body, "timed", "HEAD observer sees representation even though no payload is sent");
            } else if (entry.path == "/echo") { CheckEq(entry.body, "abc", "Expect logs only the final representation"); }
            else if (entry.path == "/missing") { CheckEq(entry.status, Status::NotFound, "404 access status"); }
            else if (entry.path == "/fail" || entry.path == "/large") {
                CheckEq(entry.status, Status::InternalServerError, "exception or output-bound fallback logs actual final status");
            } else if (entry.path == "/") { CheckEq(entry.status, Status::HttpVersionNotSupported, "fatal head logs sanitized request"); }
        }
        CheckEq(timed, std::size_t{ 3 }, "both keep-alive GETs and HEAD are logged");
    }

    /**
     * @brief Verify a throwing observer cannot prevent a successor on the same persistent connection.
     * @param executor Fixture strand.
     * @param endpoint Owned dynamic endpoint.
     */
    auto server_throwing_access_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        auto wire = co_await server_exchange(executor, std::move(endpoint),
            "GET / HTTP/1.1\r\nHost: local\r\n\r\nGET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "observed", "throwing observer leaves first response intact");
        CheckEq(parse_session_reply(wire, offset).body, "observed", "throwing observer preserves persistence");
        CheckEq(offset, wire.size(), "no HTTP error response caused by observer failure");
    }

    /**
     * @brief Isolate access logger exceptions and preserve listener behavior.
     */
    auto server_access_exception_isolation() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start throwing observer runtime");
        server::HttpServer server{ runtime };
        std::atomic<int> calls{};
        std::atomic<int> failures{};
        Check(server.Get("/", [](auto&, auto& response) {
            Check(response.SetContent("observed", "text/plain").has_value(), "observed response");
        }).has_value(), "register observed route");
        Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++calls; throw 42; }).has_value(),
            "configure throwing observer");
        Check(server.SetLogger([&](auto const& record) {
            CheckEq(record.event, server::LogEvent::AccessLoggerFailure, "access exceptions reach diagnostics");
            CheckEq(record.error, Error::Unknown, "callback diagnostic category");
            Check(record.exception && record.remote && record.local, "callback diagnostic retains exception and endpoints");
            ++failures;
            throw 7;
        }).has_value(), "configure a diagnostic callback that also throws");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind throwing observer listener");
        Check(server.Start().has_value(), "start throwing observer listener");
        TransportRuntime peers;
        peers.Run(server_throwing_access_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.IsRunning(), "observer exception does not stop listener");
        Check(server.Stop().has_value(), "drain throwing observers");
        CheckEq(calls.load(), 2, "throwing access observer runs once per complete final response");
        CheckEq(failures.load(), 2, "throwing diagnostics are contained without recursive calls");
    }

    /**
     * @brief Exercise terminal faults and normal closes without depending on public network services.
     * @param executor Independent fixture strand.
     * @param endpoint Owned listener endpoint.
     */
    auto server_diagnostic_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (auto path : { "/throw", "/post-fail", "/large", "/access-fail", "/ok" }) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", path));
            if (path == std::string_view{ "/post-fail" }) {
                Check(wire.empty(), "failed post hook produces no response");
                continue;
            }
            std::size_t offset{};
            auto const expected = path == std::string_view{ "/throw" } || path == std::string_view{ "/large" }
                ? Status::InternalServerError : Status::Ok;
            CheckEq(parse_session_reply(wire, offset).head.status, expected, "diagnostics preserve HTTP fault policy");
        }
        auto wire = co_await server_exchange(executor, endpoint, "GET / HTTP/1.2\r\nHost: local\r\n\r\n");
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).head.status, Status::HttpVersionNotSupported, "logged protocol rejection");
        auto truncated = co_await server_peer(executor, endpoint);
        std::string const input = "POST /body HTTP/1.1\r\nHost: local\r\nContent-Length: 3\r\n\r\na";
        co_await asio::async_write(*truncated, asio::buffer(input), asio::use_awaitable);
        truncated->shutdown(asio::socket_base::shutdown_send);
        wire = co_await receive_server(std::move(truncated));
        offset = 0;
        CheckEq(parse_session_reply(wire, offset).head.status, Status::BadRequest, "logged truncated body rejection");
        auto idle = co_await server_peer(executor, endpoint);
        idle->shutdown(asio::socket_base::shutdown_send);
        Check((co_await receive_server(std::move(idle))).empty(), "normal idle EOF has no response or diagnostic");
        auto timed = co_await server_peer(executor, endpoint);
        Check((co_await receive_server(std::move(timed))).empty(), "idle read deadline closes without HTTP output");
    }

    /**
     * @brief Verify diagnostic records own their metadata and distinguish protocol, transport, and callback faults.
     */
    auto server_diagnostic_observation() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start diagnostic observation runtime");
        server::HttpServer server{ runtime, { .read_timeout = 250ms, .max_response_bytes = 512 } };
        Check(server.Get("/throw", [](auto&, auto&) { throw std::runtime_error{ "private diagnostic detail" }; }).has_value(),
            "register diagnosed handler fault");
        Check(server.Get("/large", [](auto&, auto& response) {
            Check(response.SetContent(std::string(2048, 'x'), "text/plain").has_value(), "oversized diagnostic response");
        }).has_value(), "register diagnosed output bound");
        Check(server.Get("/ok", [](auto&, auto& response) {
            Check(response.SetContent("ok", "text/plain").has_value(), "healthy diagnostic sentinel");
        }).has_value(), "register healthy diagnostic sentinel");
        Check(server.Get("/access-fail", [](auto&, auto&) {}).has_value(), "register failing access observer route");
        Check(server.Post("/body", [](auto&, auto&) {}).has_value(), "register truncated upload route");
        Check(server.SetPostRoutingHandler([](auto& request, auto&) {
            if (request.Path() == "/post-fail") { throw 7; }
        }).has_value(), "register diagnosed post failure");
        Check(server.SetAccessLogger([](auto const& request, auto const&, auto) {
            if (request.Path() == "/access-fail") { throw 42; }
        }).has_value(), "diagnose a selected access callback failure");
        std::mutex mutex;
        std::vector<server::LogRecord> records;
        Check(server.SetLogger([&](auto const& record) {
            Check(Runtime::IsIoThread(), "session diagnostics execute on I/O execution");
            Check(server.LocalEndpoint().has_value(), "diagnostic callbacks may read configuration outside its mutex");
            CheckEq(server.SetLogger({}).error(), Error::AlreadyRunning, "active diagnostic callback identity is frozen");
            std::lock_guard lock{ mutex };
            records.push_back(record);
        }).has_value(), "copy owned diagnostic records for inspection after drain");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind diagnostic listener");
        auto const local = *server.LocalEndpoint();
        Check(server.Start().has_value(), "start diagnostic listener");
        TransportRuntime peers;
        peers.Run(server_diagnostic_scenario(peers.Executor(), local));
        Check(server.Stop().has_value(), "drain diagnostics before inspection");
        Check(server.SetLogger({}).has_value(), "release configured diagnostics before inspecting owned records");
        CheckEq(records.size(), std::size_t{ 7 }, "only terminal faults and access failure are diagnosed");
        std::size_t protocol{}, truncated{}, timeout{}, output{}, callbacks{}, accesses{};
        bool handler_exception{}, post_exception{}, access_exception{};
        for (auto const& record : records) {
            CheckEq(record.local, std::optional{ local }, "record retains endpoint after listener teardown");
            Check(record.remote && record.remote->address == "127.0.0.1" && record.remote->port != 0,
                "session record owns established peer endpoint");
            Check(!record.native_error, "portable session operations do not fabricate a native error");
            if (record.event == server::LogEvent::AccessLoggerFailure) { ++accesses; }
            else { CheckEq(record.event, server::LogEvent::SessionFailure, "terminal session diagnostic context"); }
            switch (record.error) {
            case Error::ProtocolError: ++protocol; break;
            case Error::ConnectionClosed: ++truncated; break;
            case Error::ReadTimeout: ++timeout; break;
            case Error::BodyTooLarge: ++output; break;
            case Error::Unknown: ++callbacks; break;
            default: Check(false, "unexpected diagnostic reason"); break;
            }
            if (record.exception) {
                try { std::rethrow_exception(record.exception); }
                catch (std::runtime_error const& error) { handler_exception = error.what() == std::string_view{ "private diagnostic detail" }; }
                catch (int value) { post_exception = post_exception || value == 7; access_exception = access_exception || value == 42; }
                catch (...) { Check(false, "unexpected retained callback exception"); }
            }
        }
        Check(protocol == 1 && truncated == 1 && timeout == 1 && output == 1 && callbacks == 3 && accesses == 1,
            "typed records distinguish every exercised fault without duplicate session logging");
        Check(handler_exception && post_exception && access_exception, "owned exception pointers retain original callback failures");
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
            { "listener bound-only stop", server_bound_only_stop },
            { "listener route paths", server_route_paths },
            { "listener route methods", server_route_methods },
            { "listener handler order", server_handler_order },
            { "listener exception concurrency", server_exception_concurrency },
            { "listener request metadata", server_request_metadata },
            { "listener request bodies", server_request_bodies },
            { "listener response mutations", server_response_mutations },
            { "listener rejected input", server_rejected_input },
            { "listener keep-alive acceptance", server_keep_alive },
            { "listener concurrent pipelines", server_concurrent_pipelines },
            { "listener Offload isolation", server_offload_isolation },
            { "listener slow header", server_slow_header },
            { "listener slow reader", server_slow_reader },
            { "listener in-flight stop", server_inflight_stop },
            { "listener Offload drain", server_offload_drain },
            { "listener callback isolation", server_callback_isolation },
            { "listener Listen execution", server_listen_execution },
            { "listener connection limit", server_connection_limit },
            { "listener socket options", server_listener_socket_options },
            { "listener accepted socket options", server_peer_socket_options },
            { "listener IPv6 dual stack", server_ipv6_and_dual_stack },
            { "listener access observation", server_access_observation },
            { "listener access exception isolation", server_access_exception_isolation },
            { "listener diagnostic observation", server_diagnostic_observation },
        });
    }

} // namespace mhttp::test
