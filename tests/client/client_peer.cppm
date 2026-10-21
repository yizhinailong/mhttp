/**
 * @file client_peer.cppm
 * @brief Provide dynamic-port raw HTTP peers that stay independent of the public client implementation.
 */
module mhttp:test.client_peer;

import std;
import asio;
import :core.parser;
import :client.http_client;
import :test.transport;

namespace mhttp::test {

    using namespace std::chrono_literals;

    /**
     * @brief Script a raw response and one optional pause without sharing any parser with the client.
     */
    struct ClientPeerPlan {
        std::string first; ///< Prefix sent after consuming the complete outgoing request head.
        std::string last; ///< Optional suffix sent after pause.
        std::chrono::milliseconds pause{}; ///< Deliberate asynchronous gap, bounded by each test.
        bool read_body{ true }; ///< False leaves a large upload blocked for write deadline tests.
    };

    /**
     * @brief Consume a request head one byte at a time so a nonreading-body peer cannot accidentally drain uploads.
     * @param socket Peer socket, retained by the owning scenario.
     * @return Owned complete header bytes, bounded independently of the library parser.
     */
    auto read_client_head(asio::ip::tcp::socket& socket) -> asio::awaitable<std::string> {
        std::string head;
        while (!head.ends_with("\r\n\r\n")) {
            if (head.size() == 128 * 1024) { throw std::runtime_error{ "outgoing client head is unbounded" }; }
            char byte{};
            co_await asio::async_read(socket, asio::buffer(&byte, 1), asio::use_awaitable);
            head += byte;
        }
        co_return head;
    }

    /**
     * @brief Accept one outgoing request, verify its framing, and emit independently scripted raw response bytes.
     * @param listener Dynamic IPv4 loopback listener, initialized before the worker starts.
     * @param plan Owned response script.
     * @param observed Request snapshot, borrowed until the coordinator joins this scenario.
     */
    auto serve_client_once(
        std::shared_ptr<asio::ip::tcp::acceptor> listener,
        ClientPeerPlan plan,
        std::string& observed
    ) -> asio::awaitable<void> {
        auto executor = co_await asio::this_coro::executor;
        asio::ip::tcp::socket socket{ executor };
        co_await listener->async_accept(socket, asio::use_awaitable);
        socket.set_option(asio::ip::tcp::no_delay{ true });
        if (!plan.read_body) { socket.set_option(asio::socket_base::receive_buffer_size{ 1024 }); }
        observed = co_await read_client_head(socket);
        Check(observed.contains(std::format("Host: 127.0.0.1:{}\r\n", listener->local_endpoint().port())),
            "outgoing Host identifies the actual numeric destination and dynamic port");
        Check(!observed.contains("Accept-Encoding:"), "default client does not advertise unavailable decoders");
        auto const marker = observed.find("Content-Length: ");
        if (marker == std::string::npos) { throw std::runtime_error{ "missing authoritative outgoing length" }; }
        auto value = std::string_view{ observed }.substr(marker + 16);
        value = value.substr(0, value.find("\r\n"));
        auto length = utils::parse_integer<std::size_t>(value);
        if (!length || *length > 16 * 1024 * 1024) { throw std::runtime_error{ "invalid outgoing body bound" }; }
        if (plan.read_body && *length) {
            std::string body(*length, '\0');
            co_await asio::async_read(socket, asio::buffer(body), asio::use_awaitable);
            observed += body;
        }
        if (!plan.first.empty()) {
            auto [error, count] = co_await asio::async_write(socket, asio::buffer(plan.first), asio::as_tuple(asio::use_awaitable));
            if (error) { co_return; }
        }
        if (plan.pause > 0ms) {
            asio::steady_timer timer{ executor };
            timer.expires_after(plan.pause);
            co_await timer.async_wait(asio::use_awaitable);
        }
        if (!plan.last.empty()) {
            static_cast<void>(co_await asio::async_write(socket, asio::buffer(plan.last), asio::as_tuple(asio::use_awaitable)));
        }
        asio::error_code ignored;
        socket.shutdown(asio::socket_base::shutdown_send, ignored);
    }

    /**
     * @brief Own a returned response independently of the fake peer, client, and both runtimes.
     */
    struct ClientPeerResult {
        Result<client::Response> response; ///< Owned response or operation error.
        std::string request; ///< Complete observed request head and consumed body.
        std::chrono::steady_clock::duration elapsed; ///< Client completion time, excluding peer cleanup wait.
        bool open{}; ///< Published connection state immediately after Send completes.
    };

    /**
     * @brief Run the raw peer on independent I/O threads and wait for public Send from this non-I/O caller.
     * @param plan Owned response script.
     * @param options Buffered client limits and phase deadlines.
     * @param request Owned outgoing request.
     * @return Complete owned observation after every peer operation and coroutine owner has drained.
     */
    auto run_client_peer(
        ClientPeerPlan plan,
        client::ClientOptions options = {},
        client::Request request = client::Request{}
    ) -> ClientPeerResult {
        TransportRuntime peer_runtime;
        auto listener = std::make_shared<asio::ip::tcp::acceptor>(peer_runtime.Executor(),
            asio::ip::tcp::endpoint{ asio::ip::address_v4::loopback(), 0 });
        auto const port = listener->local_endpoint().port();
        std::string observed;
        std::exception_ptr failure;
        std::jthread worker{ [&peer_runtime, listener, plan = std::move(plan), &observed, &failure]() mutable {
            try { peer_runtime.Run(serve_client_once(listener, std::move(plan), observed)); }
            catch (...) { failure = std::current_exception(); }
        } };
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start public client against raw peer");
        client::HttpClient client{ runtime, "127.0.0.1", port, std::move(options) };
        auto const started = std::chrono::steady_clock::now();
        auto response = SyncWait(runtime, client.Send(std::move(request)));
        auto const elapsed = std::chrono::steady_clock::now() - started;
        bool const open = client.IsOpen();
        worker.join();
        if (failure) { std::rethrow_exception(failure); }
        return { std::move(response), std::move(observed), elapsed, open };
    }

} // namespace mhttp::test
