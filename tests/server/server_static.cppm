/**
 * @file server_static.cppm
 * @brief Exercise rooted file streams against raw peers, platform links, mutation, and shutdown.
 */
module mhttp:test.server_static;

import std;
import asio;
import :server.http_server;
import :core.impl.bridge;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Locate the isolated tree, including native links, prepared by the portable runner.
     * @return Owned native fixture parent or a fixture exception.
     */
    auto static_fixture() -> std::filesystem::path {
        auto directory = std::filesystem::path{ __FILE__ }.parent_path().parent_path().parent_path().parent_path();
        return directory / "static-fixtures";
    }

    /**
     * @brief Verify raw encoded paths never escape, including native directory links and prefix collisions.
     * @param executor Independent raw-peer strand.
     * @param endpoint Dynamic loopback listener.
     */
    auto server_static_paths_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (auto target : { "/files/../outside/secret.txt", "/files/%2e%2e/outside/secret.txt",
            "/files/.%2e/outside/secret.txt", "/files/dir/../../outside/secret.txt", "/files/%2F../outside/secret.txt",
            "/files/dir%2f..%2f..%2foutside%2fsecret.txt", "/files/%5c..%5coutside%5csecret.txt",
            "/files/dir\\..\\..\\outside\\secret.txt", "/files/%00letters.txt", "/files/letters.txt:secret",
            "/files/letters.txt%3A%3A$DATA", "/files/dir./index.html", "/files/dir%20/index.html", "/files//letters.txt",
            "//files/letters.txt", "/files/CON", "/files/cOn.txt", "/files/COM1", "/files/LPT9.txt",
            "/files/escape/secret.txt", "/files/internal-link/index.html", "/files/prefix-escape/secret.txt",
            "/files/dir/index.html/child", "/files/fifo" }) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", target));
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, Status::NotFound, std::format("unsafe or unsupported path: {}", target));
            Check(!reply.body.contains("SECRET") && offset == wire.size(), "no external content or hidden framing");
        }
        if (std::filesystem::exists(static_fixture() / "public/file-link")) {
            auto wire = co_await server_exchange(executor, endpoint,
                "GET /files/file-link HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
            std::size_t offset{};
            CheckEq(parse_session_reply(wire, offset).head.status, Status::NotFound, "regular-file symlink escape rejected");
        }
        for (auto const& [target, expected] : std::vector<std::pair<std::string, std::string>>{
            { "/files/%252e%252e.txt", "decoded once" }, { "/root-link/", "safe directory" } }) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", target));
            std::size_t offset{};
            CheckEq(parse_session_reply(wire, offset).body, expected, "one decoding pass and explicitly configured root links");
        }
    }

    /**
     * @brief Run traversal and native-link containment against the public mount API.
     */
    auto server_static_paths() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start raw static runtime");
        server::HttpServer server{ runtime };
        Check(server.Mount("/files", static_fixture() / "public").has_value(), "mount raw file tree");
        Check(server.Mount("/root-link", static_fixture() / "public/internal-link").has_value(), "trusted configured root link");
        Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start raw static listener");
        TransportRuntime peers;
        peers.Run(server_static_paths_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain native path handles");
    }

    /**
     * @brief Verify fixed-length file/HEAD pipelines for both HTTP versions and raw optional whitespace.
     * @param executor Independent peer strand.
     * @param endpoint Dynamic listener endpoint.
     */
    auto server_static_framing_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        auto wire = co_await server_exchange(executor, endpoint,
            "GET /files/letters.txt HTTP/1.0\r\nHost: local\r\nConnection: keep-alive\r\n\r\n"
            "HEAD /files/letters.txt HTTP/1.0\r\nHost: local\r\nConnection: keep-alive\r\nRange: bytes=999-\r\n\r\n"
            "GET /after HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        std::size_t offset{};
        auto first = parse_session_reply(wire, offset);
        auto head = parse_session_reply(wire, offset, Method::Head);
        auto last = parse_session_reply(wire, offset);
        Check(first.head.version == Version::Http10 && first.body == "abcdefghijklmnopqrstuvwxyz"
            && first.head.headers.Get("Content-Length") == "26" && first.head.headers.Get("Connection") == "keep-alive",
            "HTTP/1.0 static file uses exact framing and explicit persistence");
        Check(head.head.status == Status::Ok && head.body.empty() && head.head.headers.Get("Content-Length") == "26",
            "HEAD ignores unsatisfiable ranges and preserves representation length");
        Check(last.body == "after" && offset == wire.size(), "pipelined successor begins only after complete fixed output");
        auto ows = co_await server_exchange(executor, endpoint,
            "GET /files/letters.txt HTTP/1.1\r\nHost: local\r\nRange: \tbytes= 0-0, 25-25 \t\r\nConnection: close\r\n\r\n");
        offset = 0;
        auto multipart = parse_session_reply(ows, offset);
        Check(multipart.head.status == Status::PartialContent && multipart.body.contains("bytes 0-0/26")
            && multipart.body.contains("bytes 25-25/26"), "wire OWS is accepted before bounded range parsing");
    }

    /**
     * @brief Run exact static framing against a peer that pipelines HTTP/1.0 and HTTP/1.1 requests.
     */
    auto server_static_framing() -> void {
        Runtime runtime;
        Check(runtime.Start(2).has_value(), "start static framing runtime");
        server::HttpServer server{ runtime, { .max_response_bytes = 512, .stream_chunk_bytes = 5 } };
        Check(server.Mount("/files", static_fixture() / "public").has_value(), "static framing mount");
        Check(server.Get("/after", [](auto&, auto& response) { static_cast<void>(response.SetContent("after", "text/plain")); }).has_value(),
            "framing successor probe");
        Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start static framing listener");
        TransportRuntime peers;
        peers.Run(server_static_framing_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain static framing");
    }

    /**
     * @brief Read only the final head while preserving an unread response body for cancellation tests.
     * @param peer Owned independent raw socket.
     * @return Exact final head bytes.
     */
    auto receive_static_head(std::shared_ptr<asio::ip::tcp::socket> peer) -> asio::awaitable<std::string> {
        std::string head;
        char byte{};
        while (!head.ends_with("\r\n\r\n")) {
            if (head.size() == 4096) { throw std::runtime_error{ "static head exceeded fixture bound" }; }
            co_await asio::async_read(*peer, asio::buffer(&byte, 1), asio::use_awaitable);
            head += byte;
        }
        co_return head;
    }

    /**
     * @brief Stop or reset a file stream whose peer stops reading after the head.
     * @param executor Independent peer strand.
     * @param endpoint Dynamic listener endpoint.
     * @param server Listener retained throughout the fixture coroutine.
     * @param mode Zero resets, one stops, and two waits for the whole-response write deadline.
     * @param diagnostics Completed failure notifications from the drained session.
     */
    auto server_static_stop_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        server::HttpServer& server,
        unsigned mode,
        std::atomic<unsigned>& diagnostics
    ) -> asio::awaitable<void> {
        auto peer = co_await server_peer(executor, endpoint);
        peer->set_option(asio::socket_base::receive_buffer_size{ 1024 });
        std::string const input = "GET /files/large.bin HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        auto head = co_await receive_static_head(peer);
        Check(head.contains("Content-Length: 33554432\r\n"), "large file head commits before complete reading");
        if (mode == 0) { peer->set_option(asio::socket_base::linger{ true, 0 }); peer->close(); }
        if (mode == 1) {
            asio::steady_timer pause{ executor, 30ms };
            co_await pause.async_wait(asio::use_awaitable);
            co_await impl::AwaitTask(server.AsyncStop(), executor);
        } else {
            auto const deadline = std::chrono::steady_clock::now() + 1s;
            while (diagnostics == 0 && std::chrono::steady_clock::now() < deadline) {
                asio::steady_timer pause{ executor, 1ms };
                co_await pause.async_wait(asio::use_awaitable);
            }
        }
    }

    /**
     * @brief Cancel large files during unread output and prove the last file handle retires before Stop returns.
     */
    auto server_static_cleanup() -> void {
        for (unsigned mode{}; mode != 3; ++mode) {
            Runtime runtime;
            Check(runtime.Start(4).has_value(), "start static cleanup runtime");
            std::atomic<unsigned> completed{}, diagnostics{};
            server::HttpServer server{ runtime, { .write_timeout = 150ms, .max_response_bytes = 65536, .stream_chunk_bytes = 65536 } };
            Check(server.Mount("/files", static_fixture() / "public").has_value(), "cleanup mount");
            Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++completed; }).has_value(), "observe successful file completion");
            Check(server.SetLogger([&](auto const&) { ++diagnostics; }).has_value(), "observe file failures");
            Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start static cleanup listener");
            TransportRuntime peers;
            peers.Run(server_static_stop_scenario(peers.Executor(), *server.LocalEndpoint(), server, mode, diagnostics));
            Check(server.Stop().has_value(), "join static reset or stop");
            Check(completed == 0, "interrupted file is never recorded as a complete response");
            if (mode != 1) { Check(diagnostics != 0, "peer reset or unread response timeout is diagnosed after drain"); }
            auto const file = static_fixture() / "public/large.bin";
            auto const renamed = static_fixture() / "public/retired.bin";
            std::error_code error;
            std::filesystem::rename(file, renamed, error);
            Check(!error, "native file handle no longer pins the file after provider drain");
            if (!error) { std::filesystem::rename(renamed, file); }
        }
    }

    /**
     * @brief Retain a raw mutation reply until the peer fixture completes.
     * @param executor Independent raw-peer strand.
     * @param endpoint Dynamic listener endpoint.
     * @param wire Borrowed output alive through this complete fixture coroutine.
     */
    auto server_static_mutation_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::string& wire
    ) -> asio::awaitable<void> {
        wire = co_await server_exchange(executor, endpoint,
            "GET /files/mutable.txt HTTP/1.1\r\nHost: local\r\n\r\n"
            "GET /after HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
    }

    /**
     * @brief Mutate a selected file before its provider starts and verify exact framing fails closed.
     */
    auto server_static_mutation() -> void {
        auto const file = static_fixture() / "public/mutable.txt";
        { std::ofstream output{ file, std::ios::binary }; output << "original representation"; }
        Runtime runtime;
        Check(runtime.Start(2).has_value(), "start static mutation runtime");
        std::atomic<unsigned> after{}, diagnostics{};
        server::HttpServer server{ runtime };
        Check(server.Mount("/files", static_fixture() / "public").has_value(), "mutable static mount");
        Check(server.SetPostRoutingHandler([file](auto& request, auto&) -> Task<void> {
            if (request.Path() == "/files/mutable.txt") {
                co_await Offload([file] { std::ofstream output{ file, std::ios::binary }; output << "x"; });
            }
        }).has_value(), "mutate selected file on a worker before head commitment");
        Check(server.Get("/after", [&](auto&, auto&) { ++after; }).has_value(), "mutation pipeline probe");
        Check(server.SetLogger([&](auto const& event) { if (event.error == Error::Read) { ++diagnostics; } }).has_value(),
            "observe native mutation detection");
        Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start static mutation listener");
        TransportRuntime peers;
        std::string wire;
        peers.Run(server_static_mutation_scenario(peers.Executor(), *server.LocalEndpoint(), wire));
        Check(wire.starts_with("HTTP/1.1 200 OK\r\n") && wire.ends_with("\r\n\r\n")
            && wire.contains("Content-Length: 23\r\n"), "mutation leaves only the original final head and closes incomplete body");
        Check(server.Stop().has_value(), "drain failed file read");
        Check(after == 0 && diagnostics == 1, "file read error does not replay a successor or publish a second response");
        Check(std::filesystem::remove(file), "failed stream releases its native file handle");
    }

    /**
     * @brief Repeatedly request a path while a foreign thread exchanges its directory with an escaping link.
     * @param executor Independent peer strand.
     * @param endpoint Dynamic listener endpoint.
     */
    auto server_static_race_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (unsigned i{}; i != 80; ++i) {
            auto wire = co_await server_exchange(executor, endpoint,
                "GET /files/racing/secret.txt HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            Check(reply.head.status == Status::NotFound || (reply.head.status == Status::Ok && reply.body == "safe"),
                "concurrent directory/link replacement returns only allowed bytes or a closed fallback");
            Check(!wire.contains("SECRET"), "link swap never exposes the external representation");
        }
    }

    /**
     * @brief Exercise the checked-open race with real symlinks or Windows junctions rather than pathname mocks.
     */
    auto server_static_race() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start static race runtime");
        server::HttpServer server{ runtime };
        auto const base = static_fixture() / "public";
        Check(server.Mount("/files", base).has_value(), "race mount");
        Check(server.BindToAnyPort("127.0.0.1").has_value() && server.Start().has_value(), "start static race listener");
        std::atomic<unsigned> exchanges{};
        std::atomic<bool> failed{};
        std::jthread swapper{ [&](std::stop_token stop) {
            auto rename = [&](auto const& from, auto const& to) {
                auto const deadline = std::chrono::steady_clock::now() + 2s;
                for (;;) {
                    std::error_code error;
                    std::filesystem::rename(from, to, error);
                    if (!error) { return true; }
                    if (std::chrono::steady_clock::now() >= deadline) { failed = true; return false; }
                    std::this_thread::yield();
                }
            };
            while (!stop.stop_requested()) {
                if (!rename(base / "racing", base / "racing-park")
                    || !rename(base / "racing-link", base / "racing")
                    || !rename(base / "racing", base / "racing-link")
                    || !rename(base / "racing-park", base / "racing")) { return; }
                ++exchanges;
            }
        } };
        TransportRuntime peers;
        peers.Run(server_static_race_scenario(peers.Executor(), *server.LocalEndpoint()));
        swapper.request_stop();
        swapper.join();
        Check(!failed && exchanges != 0, "native directory/link exchanges completed during requests");
        Check(server.Stop().has_value(), "drain race handles");
    }

} // namespace mhttp::test
