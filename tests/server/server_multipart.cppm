/**
 * @file server_multipart.cppm
 * @brief Verify bytewise multipart uploads, persistence, truncation, deadlines, and cleanup through raw loopback peers.
 */
module mhttp:test.server_multipart;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    using namespace std::string_view_literals;

    /**
     * @brief Own an independent MIME fixture with preamble, repeated file fields, an escaped quote, and NUL content.
     * @return Complete raw body, independent of the production encoder.
     */
    auto raw_multipart_body() -> std::string {
        return std::string{ "preamble\r\n--raw\r\nContent-Disposition: form-data; name=title\r\n\r\nexample\r\n"
            "--raw\r\nContent-Disposition: form-data; name=files; filename=\"a\\\"b.bin\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\na\0b\r\n"
            "--raw\r\nContent-Disposition: form-data; name=files; filename=second.bin\r\n\r\nsecond\r\n"
            "--raw--\r\nepilogue"sv };
    }

    /**
     * @brief Send multipart bytes individually, then probe incomplete uploads on fresh owned sockets.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback destination.
     * @param stop_ready Signal a foreign stopping thread only after the final incomplete upload is issued.
     */
    auto server_multipart_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::latch& stop_ready
    ) -> asio::awaitable<void> {
        auto body = raw_multipart_body();
        auto head = std::format("POST /upload HTTP/1.1\r\nHost: local\r\n"
            "Content-Type: multipart/form-data; charset=utf-8; boundary=\"raw\"\r\nContent-Length: {}\r\n\r\n", body.size());
        auto peer = co_await server_peer(executor, endpoint);
        peer->set_option(asio::ip::tcp::no_delay{ true });
        co_await asio::async_write(*peer, asio::buffer(head), asio::use_awaitable);
        for (char const& byte : body) {
            co_await asio::async_write(*peer, asio::buffer(&byte, 1), asio::use_awaitable);
            co_await asio::post(executor, asio::use_awaitable);
        }
        auto wire = co_await receive_one_server(peer);
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "uploaded", "bytewise body reaches a complete multipart handler");
        auto second = std::format("POST /upload HTTP/1.1\r\nHost: local\r\nContent-Type: multipart/form-data; boundary=raw\r\n"
            "Content-Length: {}\r\nConnection: close\r\n\r\n{}", body.size(), body);
        co_await asio::async_write(*peer, asio::buffer(second), asio::use_awaitable);
        wire = co_await receive_server(peer);
        offset = 0;
        CheckEq(parse_session_reply(wire, offset).body, "uploaded", "complete upload leaves the framed connection reusable");
        CheckEq(offset, wire.size(), "one response follows the reused upload");

        for (bool half_close : { true, false }) {
            auto partial = co_await server_peer(executor, endpoint);
            auto input = head + body.substr(0, 3);
            co_await asio::async_write(*partial, asio::buffer(input), asio::use_awaitable);
            if (half_close) { partial->shutdown(asio::socket_base::shutdown_send); }
            wire = co_await receive_server(partial);
            offset = 0;
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, half_close ? Status::BadRequest : Status::RequestTimeout,
                "incomplete multipart HTTP intake returns 400 on EOF or 408 on deadline");
            CheckEq(reply.head.headers.Get("Connection"), "close", "incomplete uploads never retain their connection");
            CheckEq(offset, wire.size(), "incomplete upload receives exactly one final response");
        }
        auto canceled = co_await server_peer(executor, std::move(endpoint));
        auto input = head + body.substr(0, 3);
        co_await asio::async_write(*canceled, asio::buffer(input), asio::use_awaitable);
        stop_ready.count_down();
        Check((co_await receive_server(std::move(canceled))).empty(), "server Stop cancels pending upload intake without dispatch or an error response");
    }

    /**
     * @brief Contain incomplete upload lifetimes and release route/body owners before listener stop returns.
     */
    auto server_multipart_input() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start raw multipart runtime");
        server::HttpServer server{ runtime, { .read_timeout = 1s } };
        std::atomic<unsigned> calls{};
        Check(server.Post("/upload", [&calls](auto& request, auto& response) {
            ++calls;
            auto form = request.Multipart();
            if (Check(form.has_value(), "raw fragmented upload is valid multipart")) {
                CheckEq(form->Size(), std::size_t{ 3 }, "raw preamble is excluded from the three ordered parts");
                CheckEq(form->Count("files"), std::size_t{ 2 }, "raw repeated file names remain distinct uploads");
                CheckEq(form->Get("title")->content, "example", "raw field value survives segmentation");
                CheckEq(form->Parts()[1].content, "a\0b"sv, "raw file keeps embedded NUL bytes");
                CheckEq(*form->Parts()[1].filename, "a\"b.bin", "raw escaped quote does not terminate filename metadata");
            }
            Check(response.SetContent("uploaded", "text/plain").has_value(), "complete raw upload response");
        }).has_value(), "register raw multipart handler");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind raw multipart listener");
        Check(server.Start().has_value(), "start raw multipart listener");
        auto endpoint = *server.LocalEndpoint();
        std::latch stop_ready{ 1 };
        std::jthread stopper{ [&] {
            stop_ready.wait();
            Check(server.Stop().has_value(), "foreign thread stops incomplete multipart intake and drains ownership");
        } };
        TransportRuntime peers;
        peers.Run(server_multipart_scenario(peers.Executor(), std::move(endpoint), stop_ready));
        stopper.join();
        CheckEq(calls.load(), 2U, "only two complete uploads execute application parsing");
        Check(server.Get("/after", [](auto&, auto&) {}).has_value(), "stopped multipart intake releases all routing leases");
    }

} // namespace mhttp::test
