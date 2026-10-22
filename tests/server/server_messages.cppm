/**
 * @file server_messages.cppm
 * @brief Verify request metadata, binary content, and response mutations through independent raw HTTP peers.
 */
module mhttp:test.server_messages;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    constexpr std::string_view META_TARGET{ "/meta/a%2Fb?tag=one+two&tag=three&flag&percent=%" };
    constexpr std::string_view META_BINARY{ "a\0b\0tail", 8 };

    /**
     * @brief Compare server endpoint metadata against the actual peer socket and preserve a binary query/body request.
     * @param executor Fixture strand.
     * @param endpoint Owned loopback listener endpoint.
     */
    auto server_request_metadata_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (bool spoofed : { false, true }) {
            auto peer = co_await server_peer(executor, endpoint);
            auto const local = peer->local_endpoint();
            auto input = std::format("POST {} HTTP/1.1\r\nHost: local\r\nX-Repeated: one\r\nx-repeated: two\r\n"
                "Content-Length: {}\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n{}\r\n",
                META_TARGET, META_BINARY.size(), spoofed ? "REMOTE_ADDR: 203.0.113.9\r\nLOCAL_ADDR: 203.0.113.10\r\n" : "");
            input.append(META_BINARY);
            co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
            auto wire = co_await receive_server(std::move(peer));
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, Status::Ok, "metadata request reaches handler");
            CheckEq(reply.body, META_BINARY, "query and embedded NUL body survive the same request");
            CheckEq(reply.head.headers.Get("Content-Type"), "application/octet-stream", "binary content type");
            CheckEq(reply.head.headers.Get("Content-Length"), "8", "binary body length counts NUL bytes");
            CheckEq(reply.head.headers.Get("X-Remote-Address"), local.address().to_string(), "server peer address matches actual socket");
            CheckEq(reply.head.headers.Get("X-Remote-Port"), std::to_string(local.port()), "server peer port matches actual socket");
            CheckEq(reply.head.headers.Get("X-Local-Address"), endpoint.address, "server local address matches listener");
            CheckEq(reply.head.headers.Get("X-Local-Port"), std::to_string(endpoint.port), "server local port matches listener");
            CheckEq(offset, wire.size(), "no binary suffix is lost or interpreted as another response");
        }
    }

    /**
     * @brief Verify raw target ownership, ordered query/field parsing, and endpoint separation from incoming fields.
     */
    auto server_request_metadata() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start request metadata runtime");
        server::HttpServer server{ runtime };
        Check(server.set_http_handler<Method::Post>("/meta/:name", [](auto& request, auto& response) {
            CheckEq(request.Target(), META_TARGET, "handler sees the original encoded target and query bytes");
            CheckEq(request.Path(), "/meta/a/b", "application path decodes encoded slash");
            CheckEq(request.PathParam("name"), "a/b", "encoded slash remains inside one route capture");
            CheckEq(request.Query().Values("tag"), std::vector<std::string_view>{ "one two", "three" }, "ordered repeated query values");
            Check(request.Query().Has("flag") && request.Query().Get("flag").empty(), "body request retains query flag");
            CheckEq(request.Query().Get("percent"), "%", "trailing percent query value remains literal");
            CheckEq(request.Headers().Values("X-Repeated"), std::vector<std::string_view>{ "one", "two" }, "repeated fields survive network parsing");
            CheckEq(request.Body(), META_BINARY, "handler owns complete binary body independently of query");
            CheckEq(request.ContentType(), "application/octet-stream", "request content type is retained");
            CheckEq(request.RemoteEndpoint().address, "127.0.0.1", "peer endpoint never trusts header fields");
            CheckEq(request.LocalEndpoint().address, "127.0.0.1", "local endpoint never trusts header fields");
            if (request.Headers().Has("REMOTE_ADDR")) {
                CheckEq(request.Header("REMOTE_ADDR"), "203.0.113.9", "supplied peer field remains ordinary user input");
                CheckEq(request.Header("LOCAL_ADDR"), "203.0.113.10", "supplied local field remains ordinary user input");
            } else { Check(!request.Headers().Has("LOCAL_ADDR"), "transport does not inject address fields"); }
            Check(!request.Headers().Has("REMOTE_PORT") && !request.Headers().Has("LOCAL_PORT"), "transport does not inject port fields");
            Check(response.Headers().Set("X-Remote-Address", request.RemoteEndpoint().address).has_value(), "echo peer address");
            Check(response.Headers().Set("X-Remote-Port", std::to_string(request.RemoteEndpoint().port)).has_value(), "echo peer port");
            Check(response.Headers().Set("X-Local-Address", request.LocalEndpoint().address).has_value(), "echo local address");
            Check(response.Headers().Set("X-Local-Port", std::to_string(request.LocalEndpoint().port)).has_value(), "echo local port");
            Check(response.SetContent(request.Body(), request.ContentType()).has_value(), "echo owned binary representation");
        }).has_value(), "register metadata echo route");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind metadata listener");
        Check(server.Start().has_value(), "start metadata listener");
        TransportRuntime peers;
        peers.Run(server_request_metadata_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain metadata handlers");
    }

    /**
     * @brief Verify empty body framing, arbitrary byte values, and permissive percent decoding through HTTP.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback listener endpoint.
     */
    auto server_request_bodies_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (auto method : { Method::Get, Method::Post, Method::Put }) {
            for (bool explicit_length : { false, true }) {
                auto wire = co_await server_exchange(executor, endpoint,
                    std::format("{} /echo HTTP/1.1\r\nHost: local\r\nConnection: close\r\n{}\r\n", ToString(method),
                        explicit_length ? "Content-Length: 0\r\n" : ""));
                std::size_t offset{};
                auto reply = parse_session_reply(wire, offset, method);
                CheckEq(reply.head.status, Status::Ok, "empty framed or unframed request succeeds");
                Check(reply.body.empty(), "empty request Body returns an empty value");
                CheckEq(reply.head.headers.Get("Content-Length"), "0", "empty response is explicitly framed");
                Check(!reply.head.headers.Has("Content-Type"), "untyped empty response does not invent a media type");
                CheckEq(offset, wire.size(), "empty response has no hidden payload");
            }
        }
        std::string binary;
        for (unsigned index{}; index < 4096; ++index) { binary += static_cast<char>(index % 256); }
        auto input = std::format("POST /echo HTTP/1.1\r\nHost: local\r\nContent-Length: {}\r\n"
            "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n", binary.size());
        input += binary;
        auto wire = co_await server_exchange(executor, endpoint, std::move(input));
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, binary, "all byte values round-trip without text or NUL truncation");
        CheckEq(offset, wire.size(), "binary echo emits exactly its declared length");
        std::vector<std::pair<std::string, std::string>> const decoded{
            { "end%", "end%" }, { "%GG", "%GG" }, { "a%2", "a%2" }, { "a%00b", std::string{ "a\0b", 3 } }, { "%2F", "/" },
        };
        for (auto const& [encoded, expected] : decoded) {
            wire = co_await server_exchange(executor, endpoint,
                std::format("GET /decoded/{} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", encoded));
            offset = 0;
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, Status::Ok, "percent path reaches the wildcard route");
            CheckEq(reply.body, expected, "path decoding preserves invalid escapes and embedded NUL bytes");
            CheckEq(offset, wire.size(), "decoded path body remains correctly framed");
        }
    }

    /**
     * @brief Verify binary and bodyless requests without exposing protocol parser helpers to consumers.
     */
    auto server_request_bodies() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start request bodies runtime");
        server::HttpServer server{ runtime };
        Check(server.set_http_handler<Method::Get, Method::Post, Method::Put>("/echo", [](auto& request, auto& response) {
            Check(response.SetContent(request.Body(), request.ContentType()).has_value(), "echo empty or binary body");
        }).has_value(), "register method-specific body echo");
        Check(server.set_http_handler<Method::Get>("/decoded/*", [](auto& request, auto& response) {
            Check(request.Body().empty(), "bodyless path request returns empty Body");
            Check(response.SetContent(std::string{ request.PathParam("*") }, "application/octet-stream").has_value(),
                "echo decoded path bytes");
        }).has_value(), "register decoded binary path echo");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind request bodies listener");
        Check(server.Start().has_value(), "start request bodies listener");
        TransportRuntime peers;
        peers.Run(server_request_bodies_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain request body handlers");
    }

    /**
     * @brief Describe a buffered response after every application mutation and protocol framing decision.
     */
    struct ResponseExpectation {
        std::string_view path; ///< Static origin-form target, alive for the complete test.
        Status status; ///< Expected final status.
        std::string_view body; ///< Expected final representation bytes.
    };

    /**
     * @brief Verify response mutation results and reject a pipeline successor after application-requested close.
     * @param executor Independent fixture strand.
     * @param endpoint Owned loopback listener endpoint.
     */
    auto server_response_mutations_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        std::vector<ResponseExpectation> const cases{
            { "/empty/200", Status::Ok, "" },
            { "/empty/204", Status::NoContent, "" },
            { "/error", Status::Forbidden, "403 Forbidden\n" },
            { "/safe", Status::Ok, "safe" },
            { "/redirect/301", Status::MovedPermanently, "" },
            { "/redirect/302", Status::Found, "" },
            { "/redirect/303", Status::SeeOther, "" },
            { "/redirect/307", Status::TemporaryRedirect, "" },
        };
        for (auto const& test : cases) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", test.path));
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, test.status, "mutated response final status");
            CheckEq(reply.body, test.body, "mutated response final body");
            CheckEq(reply.head.headers.Get("X-Custom"), "kept", "response mutations preserve unrelated application fields");
            CheckEq(reply.head.headers.Get("Connection"), "close", "explicit closing request disables persistence");
            Check(!reply.head.headers.Has("Transfer-Encoding"), "buffered response replaces application transfer coding");
            if (test.status == Status::NoContent) { Check(!reply.head.headers.Has("Content-Length"), "204 has no length field"); }
            else { CheckEq(reply.head.headers.Get("Content-Length"), std::to_string(test.body.size()), "wire length reflects final representation"); }
            CheckEq(reply.head.headers.Has("Content-Type"), !test.body.empty(), "empty and redirect responses clear content type");
            if (test.path.starts_with("/redirect/")) {
                CheckEq(reply.head.headers.Get("Location"), "/destination?x=one%20two", "valid redirect location survives serialization");
            } else { Check(!reply.head.headers.Has("Location"), "rejected redirect cannot add Location"); }
            Check(!wire.contains("Injected"), "rejected CRLF input never reaches wire headers or payload");
            CheckEq(offset, wire.size(), "response mutation produces exactly one complete HTTP response");
        }
        auto wire = co_await server_exchange(executor, endpoint,
            "GET /close HTTP/1.1\r\nHost: local\r\n\r\nGET /tail HTTP/1.1\r\nHost: local\r\n\r\n");
        std::size_t offset{};
        auto closing = parse_session_reply(wire, offset);
        CheckEq(closing.head.headers.Get("Connection"), "close", "handler-requested close overrides persistent request");
        CheckEq(closing.body, "closed", "closing response is completely written before FIN");
        CheckEq(offset, wire.size(), "no pipeline response follows application close");
    }

    /**
     * @brief Verify empty/error/redirect mutation, header injection rejection, authoritative framing, and close policy.
     */
    auto server_response_mutations() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start response mutation runtime");
        std::atomic<unsigned> tail{};
        server::HttpServer server{ runtime };
        Check(server.SetPreRoutingHandler([](auto&, auto& response) -> bool {
            Check(response.Headers().Set("X-Custom", "kept").has_value(), "add custom field before response mutation");
            return false;
        }).has_value(), "register custom response field before mutations");
        Check(server.set_http_handler<Method::Get>("/empty/:code", [](auto& request, auto& response) {
            Check(request.Body().empty(), "bodyless request has an empty Body");
            Check(response.SetContent("discarded", "text/html").has_value(), "prepare discarded representation");
            Check(response.SetEmpty(static_cast<Status>(*request.template PathParam<unsigned>("code"))).has_value(),
                "SetEmpty clears body and media type");
        }).has_value(), "register empty response mutation");
        Check(server.set_http_handler<Method::Get>("/error", [](auto&, auto& response) {
            Check(response.SetError(Status::Forbidden).has_value(), "SetError prepares safe standard representation");
        }).has_value(), "register error response mutation");
        Check(server.set_http_handler<Method::Get>("/redirect/:code", [](auto& request, auto& response) {
            Check(response.SetContent("discarded", "text/html").has_value(), "prepare discarded redirect representation");
            Check(response.SetRedirect("/destination?x=one%20two",
                static_cast<Status>(*request.template PathParam<unsigned>("code"))).has_value(), "set selected redirect status");
        }).has_value(), "register redirect response mutation");
        Check(server.set_http_handler<Method::Get>("/safe", [](auto&, auto& response) {
            Check(response.SetContent("safe", "text/plain").has_value(), "prepare valid response before rejected mutations");
            Check(response.Headers().Set("X-Custom", "kept").has_value(), "prepare valid custom field");
            CheckEq(response.Headers().Set("X-Custom", "bad\r\nInjected: yes").error(), Error::InvalidHeaders,
                "reject response field value splitting");
            CheckEq(response.Headers().Add("Bad\r\nInjected", "yes").error(), Error::InvalidHeaders,
                "reject response field name splitting");
            CheckEq(response.SetRedirect("/bad\r\nInjected: yes").error(), Error::InvalidHeaders,
                "reject redirect splitting without changing status or representation");
            CheckEq(response.SetContent("bad", "text/plain\r\nInjected: yes").error(), Error::InvalidHeaders,
                "reject media type splitting without changing representation");
            CheckEq(response.Header("X-Custom"), "kept", "rejected field replacement preserves previous value");
            Check(response.Headers().Set("Content-Length", "9999").has_value(), "prepare stale application length");
            Check(response.Headers().Set("Transfer-Encoding", "chunked").has_value(), "prepare application framing override");
        }).has_value(), "register rejected response mutation checks");
        Check(server.set_http_handler<Method::Get>("/close", [](auto&, auto& response) {
            Check(response.Headers().Set("Connection", "close").has_value(), "application requests close");
            Check(response.SetContent("closed", "text/plain").has_value(), "closing response representation");
        }).has_value(), "register application close response");
        Check(server.set_http_handler<Method::Get>("/tail", [&](auto&, auto&) { ++tail; }).has_value(), "register forbidden pipeline successor sentinel");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind response mutation listener");
        Check(server.Start().has_value(), "start response mutation listener");
        TransportRuntime peers;
        peers.Run(server_response_mutations_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain response mutation handlers");
        CheckEq(tail.load(), 0U, "closing response prevents execution of buffered pipeline successor");
    }

} // namespace mhttp::test
