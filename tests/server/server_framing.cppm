/**
 * @file server_framing.cppm
 * @brief Verify connection reuse and fatal input rejection through complete HTTP listener exchanges.
 */
module mhttp:test.server_framing;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    constexpr std::string_view HIDDEN_REQUEST{
        "GET /sentinel HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n" };

    /**
     * @brief Own a fatal or early-response request and its expected final status.
     */
    struct RejectedExchange {
        std::string input; ///< Owned request and any hidden pipeline bytes.
        Status status; ///< Expected single final response.
    };

    /**
     * @brief Verify response status, prompt close, and pipeline isolation for every rejected input class.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback endpoint.
     */
    auto server_rejected_input_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        std::vector<RejectedExchange> const cases{
            { "GET /" + std::string(80, 'x') + " HTTP/1.1\r\nHost: local\r\n\r\n", Status::UriTooLong },
            { "GET /?q=" + std::string(80, 'x') + " HTTP/1.1\r\nHost: local\r\n\r\n", Status::UriTooLong },
            { "GET /ok HTTP/1.1\r\nHost: local\r\nX-1: a\r\nX-2: b\r\nX-3: c\r\nX-4: d\r\n\r\n",
                Status::RequestHeaderFieldsTooLarge },
            { "GET /ok HTTP/1.1\r\nHost: local\r\nX-Large: " + std::string(130, 'x') + "\r\n\r\n",
                Status::RequestHeaderFieldsTooLarge },
            { "GET  /ok HTTP/1.1\r\nHost: local\r\n\r\n", Status::BadRequest },
            { "GET /ok HTTP/1.1\r\nHost: local\r\nBad Field: yes\r\n\r\n", Status::BadRequest },
            { "GET /ok HTTP/1.1\r\nHost: local\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\n\r\n",
                Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: +1\r\n\r\n", Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: 0x2e\r\n\r\n", Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: 1, 1\r\n\r\n", Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length:\r\n\r\n", Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: -1\r\n\r\n", Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: 1 0\r\n\r\n", Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: 18446744073709551616\r\n\r\n",
                Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
                Status::BadRequest },
            { "POST /ok HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
                Status::BadRequest },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
                Status::NotImplemented },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: gzip\r\n\r\n", Status::NotImplemented },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nContent-Length: 129\r\n\r\n" + std::string(129, 'x'),
                Status::ContentTooLarge },
            { "POST /ok HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n81\r\n"
                + std::string(129, 'x') + "\r\n0\r\n\r\n", Status::ContentTooLarge },
            { std::format("POST /missing HTTP/1.1\r\nHost: local\r\nContent-Length: {}\r\n\r\n{}",
                HIDDEN_REQUEST.size(), HIDDEN_REQUEST), Status::NotFound },
            { std::format("GET /early HTTP/1.1\r\nHost: local\r\nContent-Length: {}\r\n\r\n{}",
                HIDDEN_REQUEST.size(), HIDDEN_REQUEST), Status::Forbidden },
            { "POST /early HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n", Status::Forbidden },
        };
        for (auto const& test : cases) {
            auto input = test.input;
            if (!input.starts_with("POST /early")) { input.append(HIDDEN_REQUEST); }
            auto const started = std::chrono::steady_clock::now();
            auto wire = co_await server_exchange(executor, endpoint, std::move(input));
            Check(std::chrono::steady_clock::now() - started < 1s, "rejection and FIN precede the five-second read deadline");
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, test.status, "input rejection returns the specified HTTP status");
            CheckEq(reply.head.headers.Get("Connection"), "close", "rejected or unread input cannot reuse its connection");
            CheckEq(offset, wire.size(), "hidden body and pipeline bytes never produce another response");
        }
        for (bool const duplicate : { false, true }) {
            auto const extra = duplicate ? std::format("Content-Length: {}\r\n", HIDDEN_REQUEST.size()) : "";
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET /ok HTTP/1.1\r\nHost: local\r\nContent-Length: {}\r\n{}Connection: close\r\n\r\n{}",
                    HIDDEN_REQUEST.size(), extra, HIDDEN_REQUEST));
            std::size_t offset{};
            CheckEq(parse_session_reply(wire, offset).body, "ok", "identical lengths consume one complete body before dispatch");
            CheckEq(offset, wire.size(), "HTTP-looking bytes inside an accepted body never become a pipeline request");
        }
    }

    /**
     * @brief Verify fatal input bounds, unread-body closure, and hidden-request isolation at the public listener.
     */
    auto server_rejected_input() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start rejected input runtime");
        std::atomic<unsigned> legitimate{}, hidden{};
        server::HttpServer server{ runtime, { .read_timeout = 5s, .uri_bytes = 64, .header_bytes = 128,
            .header_fields = 4, .body_bytes = 128 } };
        auto handler = [&](auto& request, auto& response) {
            ++legitimate;
            CheckEq(request.Body(), HIDDEN_REQUEST, "accepted body retains HTTP-looking bytes as payload");
            Check(response.SetContent("ok", "text/plain").has_value(), "accepted response after input rejections");
        };
        Check(server.Get("/ok", handler).has_value(), "register GET rejection sentinel");
        Check(server.Post("/ok", handler).has_value(), "register POST rejection sentinel");
        Check(server.Get("/sentinel", [&](auto&, auto&) { ++hidden; }).has_value(), "register hidden request sentinel");
        Check(server.SetPreRoutingHandler([](auto& request, auto& response) -> bool {
            if (request.Path() != "/early") { return false; }
            Check(response.SetError(Status::Forbidden).has_value(), "prepare early response without consuming declared body");
            return true;
        }).has_value(), "register immediate unread-body rejection");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind rejected input listener");
        Check(server.Start().has_value(), "start rejected input listener");
        TransportRuntime peers;
        peers.Run(server_rejected_input_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain rejected input sessions");
        CheckEq(legitimate.load(), 2U, "only completely accepted single/equal lengths dispatch a handler");
        CheckEq(hidden.load(), 0U, "no request hidden behind rejection or inside a body is dispatched");
    }

    /**
     * @brief Verify mixed sequential persistence, count-bound closure, bodyless DELETE pipelining, and HTTP/1.0.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback endpoint.
     */
    auto server_keep_alive_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        auto peer = co_await server_peer(executor, endpoint);
        std::vector<std::pair<std::string, std::string>> const requests{
            { "GET /ok", "ok" }, { "GET /missing", "404 Not Found\n" }, { "POST /empty", "" },
            { "DELETE /delete", "deleted" }, { "DELETE /delete", "deleted" }, { "GET /ok", "ok" },
        };
        for (std::size_t index{}; index < requests.size(); ++index) {
            auto input = requests[index].first + " HTTP/1.1\r\nHost: local\r\n\r\n";
            co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
            auto wire = co_await receive_one_server(peer);
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, index == 1 ? Status::NotFound : Status::Ok, "mixed persistent status");
            CheckEq(reply.body, requests[index].second, "mixed persistent response body");
            if (index + 1 == requests.size()) {
                CheckEq(reply.head.headers.Get("Connection"), "close", "configured final response closes the connection");
            } else { Check(reply.head.headers.Get("Connection") != "close", "earlier responses preserve HTTP/1.1 persistence"); }
        }
        Check((co_await receive_server(std::move(peer))).empty(), "request-count bound closes immediately after its final response");
        auto wire = co_await server_exchange(executor, endpoint,
            "GET /ok HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "ok", "new connection serves requests after count-bound closure");
        wire = co_await server_exchange(executor, endpoint,
            "DELETE /delete HTTP/1.1\r\nHost: local\r\n\r\n"
            "DELETE /delete HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        offset = 0;
        CheckEq(parse_session_reply(wire, offset).body, "deleted", "unframed DELETE does not consume its successor");
        CheckEq(parse_session_reply(wire, offset).body, "deleted", "second unframed DELETE is dispatched independently");
        CheckEq(offset, wire.size(), "two bodyless DELETE requests produce exactly two responses");
        wire = co_await server_exchange(executor, endpoint, "GET /ok HTTP/1.0\r\n\r\n");
        offset = 0;
        auto old = parse_session_reply(wire, offset);
        CheckEq(old.head.version, Version::Http10, "HTTP/1.0 request gets HTTP/1.0 response");
        CheckEq(old.body, "ok", "HTTP/1.0 payload completes before close");
        CheckEq(old.head.headers.Get("Content-Length"), "2", "HTTP/1.0 uses explicit length framing");
        Check(!old.head.headers.Has("Transfer-Encoding"), "HTTP/1.0 never receives chunked coding");
        CheckEq(old.head.headers.Get("Connection"), "close", "HTTP/1.0 defaults to closing");
        CheckEq(offset, wire.size(), "HTTP/1.0 closes after exactly one complete response");
    }

    /**
     * @brief Exercise keep-alive and per-connection request limits at the public listener.
     */
    auto server_keep_alive() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start keep-alive acceptance runtime");
        std::atomic<unsigned> calls{};
        server::HttpServer server{ runtime, { .keep_alive_max_requests = 6 } };
        Check(server.Get("/ok", [&](auto&, auto& response) {
            ++calls;
            Check(response.SetContent("ok", "text/plain").has_value(), "persistent GET response");
        }).has_value(), "register persistent GET");
        Check(server.Post("/empty", [&](auto& request, auto& response) {
            ++calls;
            Check(request.Body().empty(), "unframed persistent POST has no body");
            Check(response.SetEmpty(Status::Ok).has_value(), "persistent empty POST response");
        }).has_value(), "register persistent empty POST");
        Check(server.Delete("/delete", [&](auto& request, auto& response) {
            ++calls;
            Check(request.Body().empty(), "unframed DELETE has no body");
            Check(response.SetContent("deleted", "text/plain").has_value(), "persistent DELETE response");
        }).has_value(), "register bodyless DELETE");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind keep-alive listener");
        Check(server.Start().has_value(), "start keep-alive listener");
        TransportRuntime peers;
        peers.Run(server_keep_alive_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain keep-alive sessions");
        CheckEq(calls.load(), 9U, "one dispatch per valid request across reuse and new connections");
    }

} // namespace mhttp::test
