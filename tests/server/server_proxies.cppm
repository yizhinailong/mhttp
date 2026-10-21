/**
 * @file server_proxies.cppm
 * @brief Verify forwarded address isolation across fragmented, pipelined, rejected, and truncated requests.
 */
module mhttp:test.server_proxies;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Check exactly one final response and its owned client/transport address observations.
     * @param wire Complete response through connection close.
     * @param status Expected final status.
     * @param address Expected resolved client IP.
     */
    auto check_proxy_reply(
        std::string const& wire,
        Status status,
        std::string_view address
    ) -> void {
        std::size_t offset{};
        auto reply = parse_session_reply(wire, offset);
        CheckEq(reply.head.status, status, "raw proxy response status");
        CheckEq(reply.head.headers.Get("X-Client-IP"), address, "raw response resolves appropriate metadata");
        CheckEq(reply.head.headers.Get("X-Peer"), "127.0.0.1", "raw peer remains the real transport address");
        CheckEq(offset, wire.size(), "one closing response cannot dispatch hidden pipeline bytes");
    }

    /**
     * @brief Send adversarial raw framing while observing trusted IPs only after a validated head.
     * @param executor Owned fixture strand.
     * @param endpoint Dynamic loopback listener endpoint.
     */
    auto server_proxies_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        auto peer = co_await server_peer(executor, endpoint);
        std::string pipeline =
            "GET /ip HTTP/1.1\r\nHost: local\r\nX-Forwarded-For: 203.0.113.7, 10.0.0.1\r\n\r\n"
            "GET /ip HTTP/1.1\r\nHost: local\r\nX-Forwarded-For: 203.0.113.1, 198.51.100.9\r\n"
            "x-forwarded-for: 10.0.0.5\r\nConnection: close\r\n\r\n";
        for (char const byte : pipeline) {
            co_await asio::async_write(*peer, asio::buffer(&byte, 1), asio::use_awaitable);
        }
        auto wire = co_await receive_server(std::move(peer));
        std::size_t offset{};
        for (auto address : { "203.0.113.7", "198.51.100.9" }) {
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, Status::Ok, "fragmented pipeline reaches both handlers");
            CheckEq(reply.body, address, "pipelined requests own independent client addresses");
            CheckEq(reply.head.headers.Get("X-Client-IP"), address, "post-routing uses the matching pipeline head");
        }
        CheckEq(offset, wire.size(), "fragmented pipeline has exactly two responses");

        std::string const prefix = "GET /ip HTTP/1.1\r\nHost: local\r\nX-Forwarded-For: 203.0.113.7\r\n";
        std::string const hidden = "GET /ip HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
        std::string fields;
        for (unsigned index{}; index != 8; ++index) { fields += std::format("X-{}: a\r\n", index); }
        for (auto const& [input, status] : std::vector<std::pair<std::string, Status>>{
                 { prefix + "Content-Length: 1\r\nContent-Length: 2\r\n\r\nx" + hidden, Status::BadRequest },
                 { prefix + "X-Large: " + std::string(700, 'x') + "\r\n\r\n" + hidden, Status::RequestHeaderFieldsTooLarge },
                 { prefix + fields + "\r\n" + hidden, Status::RequestHeaderFieldsTooLarge },
                 { prefix + "Content-Length: 9\r\n\r\n123456789" + hidden, Status::ContentTooLarge } }) {
            auto rejected = co_await server_exchange(executor, endpoint, input);
            check_proxy_reply(rejected, status, "127.0.0.1");
        }
        auto missing = co_await server_exchange(executor, endpoint,
            "GET /missing HTTP/1.1\r\nHost: local\r\nX-Forwarded-For: 203.0.113.7\r\nConnection: close\r\n\r\n");
        check_proxy_reply(missing, Status::NotFound, "203.0.113.7");
        for (bool head_ip : { false, true }) {
            auto input = std::string{ "GET /ip HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n" }
                + (head_ip ? "X-Forwarded-For: 203.0.113.7\r\n" : "")
                + "Connection: close\r\n\r\n0\r\nX-Forwarded-For: 198.51.100.9\r\n\r\n";
            auto trailer = co_await server_exchange(executor, endpoint, std::move(input));
            check_proxy_reply(trailer, Status::Ok, head_ip ? "203.0.113.7" : "127.0.0.1");
        }
        auto partial = co_await server_peer(executor, std::move(endpoint));
        auto input = prefix + "Content-Length: 3\r\n\r\nx";
        co_await asio::async_write(*partial, asio::buffer(input), asio::use_awaitable);
        partial->shutdown(asio::socket_base::shutdown_send);
        auto truncated = co_await receive_server(std::move(partial));
        check_proxy_reply(truncated, Status::BadRequest, "203.0.113.7");
    }

    /**
     * @brief Keep rejected heads sanitized while retaining trusted metadata for body-stage errors.
     */
    auto server_proxy_input() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start raw proxy runtime");
        std::atomic<unsigned> handlers{}, accesses{};
        server::HttpServer server{ runtime, { .read_timeout = 2s, .header_bytes = 512,
            .header_fields = 8, .body_bytes = 8,
            .trusted_proxies = server::TrustedProxies::Create({ "127.0.0.1", "10.0.0.0/8" }).value() } };
        Check(server.Get("/ip", [&](auto& request, auto& response) -> Task<void> {
            ++handlers;
            auto const address = request.ClientIp();
            co_await Sleep(1ms);
            Check(response.SetContent(address, "text/plain").has_value(), "raw proxy response content");
        }).has_value(), "register raw proxy endpoint");
        Check(server.SetPostRoutingHandler([](auto& request, auto& response) {
            Check(response.Headers().Set("X-Client-IP", request.ClientIp()).has_value(), "raw proxy final address");
            Check(response.Headers().Set("X-Peer", request.RemoteEndpoint().address).has_value(), "raw proxy real peer");
        }).has_value(), "observe client metadata on normal/error responses");
        Check(server.SetAccessLogger([&](auto const& request, auto const& response, auto) {
            ++accesses;
            CheckEq(response.Headers().Get("X-Client-IP"), request.ClientIp(), "error access log keeps the same resolved IP");
        }).has_value(), "observe completed raw proxy responses");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind raw proxy listener");
        Check(server.Start().has_value(), "start raw proxy listener");
        TransportRuntime peers;
        peers.Run(server_proxies_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain raw proxy listener and callbacks");
        CheckEq(handlers.load(), 4u, "only two pipeline and two trailer requests dispatch");
        CheckEq(accesses.load(), 10u, "all valid and rejected responses reach access observation once");
    }

} // namespace mhttp::test
