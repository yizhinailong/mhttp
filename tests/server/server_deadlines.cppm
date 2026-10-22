/**
 * @file server_deadlines.cppm
 * @brief Trigger real whole-header and response-write deadlines through slow raw loopback peers.
 */
module mhttp:test.server_deadlines;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Trickle an owned request one byte per interval until completion or closure.
     * @param peer Owned connected socket, shared only with one concurrent reader.
     * @param input Owned request bytes retained through every write.
     * @return Success, or Write once timeout/fixture closure prevents another byte.
     */
    auto trickle_server_peer(
        std::shared_ptr<asio::ip::tcp::socket> peer,
        std::string input
    ) -> asio::awaitable<Result<void>> {
        asio::steady_timer interval{ peer->get_executor() };
        for (char const& byte : input) {
            interval.expires_after(100ms);
            co_await interval.async_wait(asio::use_awaitable);
            auto [error, count] = co_await asio::async_write(*peer, asio::buffer(&byte, 1), asio::as_tuple(asio::use_awaitable));
            if (error || count != 1) { co_return std::unexpected{ Error::Write }; }
        }
        co_return Result<void>{};
    }

    /**
     * @brief Verify individual arriving bytes cannot renew the configured one-second request-head deadline.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback endpoint.
     */
    auto server_slow_header_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        auto peer = co_await server_peer(executor, std::move(endpoint));
        auto const started = std::chrono::steady_clock::now();
        auto sender = spawn_transport(executor, trickle_server_peer(peer, "GET / HTTP/1.1\r\nHost: local\r\n\r\n"));
        auto wire = co_await receive_server(peer);
        Check(wire.starts_with("HTTP/1.1 408 Request Timeout\r\n"), "incomplete request head receives 408");
        Check(wire.contains("Connection: close\r\n") && wire.ends_with("408 Request Timeout\n"),
            "default timeout error format is framed before connection closure");
        auto const elapsed = std::chrono::steady_clock::now() - started;
        Check(elapsed >= 750ms && elapsed < 2s, "one-second deadline covers the whole head despite arriving bytes");
        peer->close();
        auto sent = co_await sender->Wait();
        Check(!sent, "header timeout prevents completion of the trickled request");
    }

    /**
     * @brief Observe a real read timeout, one final error response, and no request handler execution.
     */
    auto server_slow_header() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start slow header runtime");
        std::atomic<unsigned> calls{}, accesses{}, timeouts{};
        server::HttpServer server{ runtime, { .read_timeout = 1s } };
        Check(server.set_http_handler<mhttp::method::Get>("/", [&](auto&, auto&) { ++calls; }).has_value(), "register slow header sentinel");
        Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(), "observe incomplete head response output");
        Check(server.SetLogger([&](auto const& record) {
            CheckEq(record.event, server::LogEvent::SessionFailure, "head timeout is a session failure");
            CheckEq(record.error, Error::ReadTimeout, "actual whole-header deadline reports ReadTimeout");
            ++timeouts;
        }).has_value(), "observe actual head timeout");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind slow header listener");
        Check(server.Start().has_value(), "start slow header listener");
        TransportRuntime peers;
        peers.Run(server_slow_header_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain timed-out header session and logger");
        Check(calls == 0 && accesses == 1 && timeouts == 1, "one timeout, no request dispatch, and one completed 408 response");
    }

    /**
     * @brief Keep the receive window small and read nothing until the server reports its response-write deadline.
     * @param executor Independent fixture strand.
     * @param endpoint Owned listener endpoint.
     * @param timeouts Synchronized counter alive through fixture and server drain.
     */
    auto server_slow_reader_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::atomic<unsigned>& timeouts
    ) -> asio::awaitable<void> {
        auto peer = std::make_shared<asio::ip::tcp::socket>(executor);
        peer->open(asio::ip::tcp::v4());
        peer->set_option(asio::socket_base::receive_buffer_size{ 1024 });
        co_await peer->async_connect({ asio::ip::make_address(endpoint.address), endpoint.port }, asio::use_awaitable);
        std::string const input = "GET /large HTTP/1.1\r\nHost: local\r\n\r\n";
        auto const started = std::chrono::steady_clock::now();
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        asio::steady_timer interval{ executor };
        auto const deadline = started + 2s;
        while (timeouts == 0 && std::chrono::steady_clock::now() < deadline) {
            interval.expires_after(5ms);
            co_await interval.async_wait(asio::use_awaitable);
        }
        auto const elapsed = std::chrono::steady_clock::now() - started;
        Check(timeouts == 1, "nonreading peer triggers actual WriteTimeout");
        Check(elapsed >= 70ms && elapsed < 2s, "response write expires near its whole-operation deadline");
        if (timeouts == 0) { peer->close(); co_return; }
        peer->set_option(asio::socket_base::receive_buffer_size{ 256 * 1024 });
        auto partial = co_await receive_server(std::move(peer));
        Check(partial.size() < 8 * 1024 * 1024, "timed-out connection closes before the full representation is transmitted");
    }

    /**
     * @brief Verify response-write timeout closes the session and cannot produce a successful access observation.
     */
    auto server_slow_reader() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start slow reader runtime");
        std::atomic<unsigned> accesses{}, timeouts{};
        auto body = std::make_shared<std::string const>(8 * 1024 * 1024, 'x');
        server::HttpServer server{ runtime, { .write_timeout = 100ms } };
        Check(server.set_http_handler<mhttp::method::Get>("/large", [body = std::move(body)](auto&, auto& response) {
            Check(response.SetContent(*body, "application/octet-stream").has_value(), "prepare bounded large response");
        }).has_value(), "register response exceeding the nonreading peer window");
        Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { ++accesses; }).has_value(), "observe incomplete large response");
        Check(server.SetLogger([&](auto const& record) {
            CheckEq(record.event, server::LogEvent::SessionFailure, "write timeout is a session failure");
            CheckEq(record.error, Error::WriteTimeout, "actual response write deadline reports WriteTimeout");
            ++timeouts;
        }).has_value(), "observe actual write timeout");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind slow reader listener");
        Check(server.Start().has_value(), "start slow reader listener");
        TransportRuntime peers;
        peers.Run(server_slow_reader_scenario(peers.Executor(), *server.LocalEndpoint(), timeouts));
        Check(server.Stop().has_value(), "drain timed-out response and logger");
        Check(timeouts == 1 && accesses == 0, "incomplete write produces one timeout and no access entry");
        Check(server.set_http_handler<mhttp::method::Get>("/after", [](auto&, auto&) {}).has_value(), "timed-out response releases routing ownership");
    }

} // namespace mhttp::test
