/**
 * @file server_routes.cppm
 * @brief Verify public route registration and encoded-segment dispatch through real HTTP exchanges.
 */
module mhttp:test.server_routes;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Describe a complete wire-level route expectation with owned request and response bytes.
     */
    struct RouteExpectation {
        std::string target; ///< Encoded origin-form target.
        std::string body; ///< Expected complete response body.
        Status status{ Status::Ok }; ///< Expected status, normally successful.
    };

    /**
     * @brief Exercise parameter boundaries, route priority, regex bounds, and percent decoding over TCP.
     * @param executor Fixture strand.
     * @param endpoint Owned loopback endpoint.
     */
    auto server_route_paths_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        std::vector<RouteExpectation> const cases{
            { "/static", "literal" },
            { "/static/", "404 Not Found\n", Status::NotFound },
            { "/missing", "404 Not Found\n", Status::NotFound },
            { "/middle/value/end", "value" },
            { "/middle//end", "" },
            { "/middle/value", "404 Not Found\n", Status::NotFound },
            { "/middle/value/wrong", "404 Not Found\n", Status::NotFound },
            { "/middle/value/end/extra", "404 Not Found\n", Status::NotFound },
            { "/last/name", "name" },
            { "/last/", "" },
            { "/last", "404 Not Found\n", Status::NotFound },
            { "/last/name/extra", "404 Not Found\n", Status::NotFound },
            { "/multi/42/a%2Fb", "42:a/b" },
            { "/multi/42/a/b", "404 Not Found\n", Status::NotFound },
            { "/regex/123", "123" },
            { "/regex/abc", "404 Not Found\n", Status::NotFound },
            { "/regex/1234", "404 Not Found\n", Status::NotFound },
            { "/catch/a//b/", "a//b/" },
            { "/priority/static", "literal" },
            { "/priority/other", "other" },
            { "/priority/static/", "slash" },
            { "/a%20b", "encoded static" },
            { "/literal:colon", "literal colon" },
            { "/literalvalue", "404 Not Found\n", Status::NotFound },
        };
        for (auto const& test : cases) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", test.target));
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, test.status, std::format("route status for {}", test.target));
            CheckEq(reply.body, test.body, std::format("route body for {}", test.target));
            CheckEq(reply.head.headers.Get("Content-Length"), std::to_string(test.body.size()), "route response framing");
            CheckEq(offset, wire.size(), "exactly one route response is written");
        }
    }

    /**
     * @brief Validate registrations before serving successful and rejected path matches on a multithreaded Runtime.
     */
    auto server_route_paths() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start route path runtime");
        server::HttpServer server{ runtime, {}, { .regex_pattern_bytes = 8, .regex_subject_bytes = 3 } };
        auto fixed = [](std::string value) {
            return [value = std::move(value)](auto&, auto& response) {
                Check(response.SetContent(value, "text/plain").has_value(), "fixed route response");
            };
        };
        auto captured = [](auto& request, auto& response) {
            auto value = request.PathParam("value", request.PathParam("*"));
            Check(response.SetContent(std::string{ value }, "text/plain").has_value(), "captured route response");
        };
        Check(server.set_http_handler<Method::Get>("/static", fixed("literal")).has_value(), "register static path");
        Check(server.set_http_handler<Method::Get>("/middle/:value/end", captured).has_value(), "register middle capture");
        Check(server.set_http_handler<Method::Get>("/last/:value", captured).has_value(), "register trailing capture");
        Check(server.set_http_handler<Method::Get>("/multi/:id/:name", [](auto& request, auto& response) {
            CheckEq(request.template PathParam<int>("id"), std::optional{ 42 }, "typed numeric capture over HTTP");
            Check(response.SetContent(std::format("{}:{}", request.PathParam("id"), request.PathParam("name")),
                "text/plain").has_value(), "consecutive parameter response");
        }).has_value(), "register consecutive typed and encoded captures");
        Check(server.set_http_handler<Method::Get>("/regex/{value:[0-9]+}", captured).has_value(), "register safe bounded regex");
        Check(server.set_http_handler<Method::Get>("/catch/*", captured).has_value(), "register wildcard remainder");
        Check(server.set_http_handler<Method::Get>("/priority/:value", captured).has_value(), "register parameter before competing static path");
        Check(server.set_http_handler<Method::Get>("/priority/static", fixed("literal")).has_value(), "register higher priority static path");
        Check(server.set_http_handler<Method::Get>("/priority/static/", fixed("slash")).has_value(), "register distinct trailing slash");
        Check(server.set_http_handler<Method::Get>("/a b", fixed("encoded static")).has_value(), "register decoded literal space");
        Check(server.set_http_handler<Method::Get>("/literal:colon", fixed("literal colon")).has_value(), "register embedded literal colon");
        for (auto path : { "/regex/{value:(a+)+}", "/regex/{value:[}", "/regex/{value:123456789}" }) {
            CheckEq(server.set_http_handler<Method::Get>(path, fixed("invalid")).error(), Error::InvalidRequest,
                "dangerous, invalid, and excessive regexes are rejected without replacing valid routes");
        }
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind route path listener");
        Check(server.Start().has_value(), "start route path listener");
        TransportRuntime peers;
        peers.Run(server_route_paths_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain route path listener");
    }

    /**
     * @brief Verify shared-path methods, deterministic Allow, implicit HEAD, and bodyless HEAD errors on the wire.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback endpoint.
     */
    auto server_route_methods_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (auto method : { Method::Get, Method::Post, Method::Put, Method::Head }) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("{} /verbs HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", ToString(method)));
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset, method);
            bool const allowed = method != Method::Put;
            CheckEq(reply.head.status, allowed ? Status::Ok : Status::MethodNotAllowed, "shared-path method dispatch");
            if (allowed) {
                CheckEq(reply.body, method == Method::Head ? "" : ToString(method), "implicit HEAD suppresses GET payload");
                CheckEq(reply.head.headers.Get("Content-Length"), method == Method::Post ? "4" : "3", "HEAD shares GET length");
                CheckEq(reply.head.reason, "OK", "successful reason phrase");
                CheckEq(reply.head.headers.Values("Content-Type").size(), std::size_t{ 1 }, "one content type on the wire");
                Check(!reply.head.headers.Has("Allow"), "successful response has no error Allow");
            } else { CheckEq(reply.head.headers.Get("Allow"), "GET, HEAD, POST", "405 advertises registered and implicit methods"); }
            CheckEq(offset, wire.size(), "HEAD leaves no hidden response payload");
        }
        auto wire = co_await server_exchange(executor, endpoint,
            "HEAD /missing HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        std::size_t offset{};
        auto missing = parse_session_reply(wire, offset, Method::Head);
        CheckEq(missing.head.status, Status::NotFound, "HEAD missing route uses 404");
        Check(missing.body.empty() && offset == wire.size(), "HEAD default error has no payload");
        CheckEq(missing.head.headers.Get("Content-Length"), "14", "HEAD error advertises GET error representation");
        Check(!missing.head.headers.Has("Allow"), "404 has no Allow field");
    }

    /**
     * @brief Verify method negotiation through the complete listener/router/serializer stack.
     */
    auto server_route_methods() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start route method runtime");
        server::HttpServer server{ runtime };
        auto handler = [](auto& request, auto& response) {
            auto method = request.Method() == Method::Head ? Method::Get : *request.Method();
            Check(response.SetContent(std::string{ ToString(method) }, "text/plain").has_value(), "shared-path method response");
        };
        Check(server.set_http_handler<Method::Get>("/verbs", handler).has_value(), "register GET with implicit HEAD");
        Check(server.set_http_handler<Method::Post>("/verbs", handler).has_value(), "register POST on the same route");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind route method listener");
        Check(server.Start().has_value(), "start route method listener");
        TransportRuntime peers;
        peers.Run(server_route_methods_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain route method listener");
    }

} // namespace mhttp::test
