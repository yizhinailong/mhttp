/**
 * @file session_tests.cppm
 * @brief Exercise private HTTP sessions with raw loopback peers and four I/O threads.
 */
export module mhttp:test.session;

import std;
import asio;
import :core.impl.io_error;
import :server.impl.session;
import :test.transport;
import :core.runtime;

namespace mhttp::test {

    /**
     * @brief Own parsed final response bytes independently of the session and peer.
     */
    struct SessionReply {
        detail::ResponseHead head; ///< Parsed status and fields.
        std::string body; ///< Complete binary body.
    };

    /**
     * @brief Parse exactly one final response, preserving the successor's offset.
     * @param wire Borrowed collected peer bytes.
     * @param offset Consumed prefix, updated through one response.
     * @param method Originating method, including HEAD suppression semantics.
     * @param end_of_stream Whether transport EOF was observed, allowing a close-delimited final response.
     * @return Owned response, throwing if the session emits invalid or incomplete HTTP.
     */
    auto parse_session_reply(
        std::string const& wire,
        std::size_t& offset,
        Method method = Method::Get,
        bool end_of_stream = false
    ) -> SessionReply {
        detail::ResponseParser parser{ method };
        SessionReply reply;
        for (;;) {
            auto step = parser.Feed(std::span<char const>{ wire.data(), wire.size() }.subspan(offset));
            offset += step.consumed;
            if (step.event == detail::ParseEvent::NeedMore && end_of_stream && offset == wire.size()) {
                step = parser.Finish();
            }
            if (step.event == detail::ParseEvent::Body) {
                reply.body.append(step.data.data(), step.data.size());
            } else if (step.event == detail::ParseEvent::Complete) {
                reply.head = parser.Head();
                return reply;
            } else if (step.event == detail::ParseEvent::Failed || step.event == detail::ParseEvent::NeedMore) {
                throw std::runtime_error{ std::format("invalid/incomplete session reply at byte {}", offset) };
            }
        }
    }

    /**
     * @brief Exchange owned raw input with a frozen session and join before thawing the router.
     * @param executor Fixture strand.
     * @param router Router borrowed until this coroutine completes.
     * @param input Owned wire request(s); the final request must cause closure.
     * @param options Session limits.
     * @param half_close Whether to signal EOF after input, exercising truncated requests.
     * @return Complete final response bytes and terminal session diagnostics.
     */
    auto session_exchange(
        asio::any_io_executor executor,
        server::Router& router,
        std::string input,
        server::ServerOptions options = {},
        bool half_close = false
    ) -> asio::awaitable<std::pair<std::string, impl::SessionResult>> {
        auto pair = co_await make_loopback(executor, { 7, options.max_response_bytes });
        Check(detail::RouterAccess::Freeze(router).has_value(), "freeze session configuration");
        auto session = impl::Session::Create(pair->connection, router, options);
        if (!session) { throw std::runtime_error{ "session creation failed" }; }
        auto completed = spawn_transport(executor, (*session)->Run());
        co_await asio::async_write(pair->peer, asio::buffer(input), asio::use_awaitable);
        if (half_close) { pair->peer.shutdown(asio::socket_base::shutdown_send); }
        auto wire = co_await receive_peer(pair);
        auto result = co_await completed->Wait();
        Check(detail::RouterAccess::Thaw(router).has_value(), "all session route leases drain before thaw");
        co_return std::pair{ std::move(wire), std::move(result) };
    }

    /**
     * @brief Verify ordered pipelining, binary body ownership, captures, endpoint metadata, and HEAD.
     * @param executor Fixture strand.
     */
    auto session_pipeline_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        server::Router router;
        std::vector<std::string> order;
        Check(router.Post("/item/:id", [&](auto& request, auto& response) -> Task<void> {
            auto const body = request.Body();
            auto const capture = std::string{ request.PathParam("id") };
            co_await Sleep(2ms);
            CheckEq(request.Body(), body, "body survives async handler suspension");
            CheckEq(capture, "a/b", "encoded slash remains one routing segment");
            CheckEq(request.Query().Get("q"), "hello world", "query decoding reaches session handler");
            Check(request.RemoteEndpoint().port != 0 && request.LocalEndpoint().port != 0,
                "session snapshots dynamic endpoints");
            CheckEq(request.RemoteEndpoint().address, "127.0.0.1", "peer address is owned");
            order.push_back("POST");
            Check(response.SetContent(body, "application/octet-stream").has_value(), "set echoed binary content");
        }).has_value(), "register async session handler");
        Check(router.Get("/next", [&](auto& request, auto& response) {
            order.emplace_back(request.MethodString());
            Check(response.SetContent("next", "text/plain").has_value(), "set GET/HEAD content");
        }).has_value(), "register fallback route");
        std::string input = "POST /item/a%2Fb?q=hello+world HTTP/1.1\r\nHost: local\r\nContent-Length: 3\r\n\r\n";
        input.append("a\0b", 3);
        input += "GET /next HTTP/1.1\r\nHost: local\r\n\r\n"
            "HEAD /next HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
        auto [wire, result] = co_await session_exchange(executor, router, std::move(input));
        CheckEq(result.responses, std::size_t{ 3 }, "three pipelined responses written");
        Check(!result.error && !result.failure, "normal pipeline has no terminal failure");
        CheckEq(order, std::vector<std::string>{ "POST", "GET", "HEAD" }, "callbacks finish in wire order");
        std::size_t offset{};
        auto first = parse_session_reply(wire, offset);
        CheckEq(first.body, std::string{ "a\0b", 3 }, "binary response is complete");
        Check(!first.head.headers.Get("Date").empty(), "automatic Date is sent");
        CheckEq(parse_session_reply(wire, offset).body, "next", "successor bytes are preserved");
        auto head = parse_session_reply(wire, offset, Method::Head);
        Check(head.body.empty(), "HEAD emits no payload");
        CheckEq(head.head.headers.Get("Content-Length"), "4", "HEAD retains representation length");
        CheckEq(head.head.headers.Get("Connection"), "close", "final response closes");
        CheckEq(offset, wire.size(), "no trailing body bytes after HEAD");
    }

    /**
     * @brief Verify chunk/trailer intake and default method/path failures over the network.
     * @param executor Fixture strand.
     */
    auto session_chunked_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        server::Router router;
        Check(router.Post("/chunk", [](auto& request, auto& response) {
            CheckEq(request.Body(), "abcde", "chunk boundaries are removed");
            CheckEq(request.Trailers().Get("X-Sum"), "yes", "trailers are complete before dispatch");
            Check(request.Header("X-Sum").empty(), "trailers never overwrite request headers");
            Check(response.SetContent("chunked", "text/plain").has_value(), "set chunk response");
        }).has_value(), "register chunk route");
        auto [wire, result] = co_await session_exchange(executor, router,
            "POST /chunk HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n"
            "2\r\nab\r\n3\r\ncde\r\n0\r\nX-Sum: yes\r\n\r\n"
            "GET /absent HTTP/1.1\r\nHost: local\r\n\r\n"
            "GET /chunk HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n");
        CheckEq(result.responses, std::size_t{ 3 }, "chunk request permits pipeline reuse");
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "chunked", "chunked handler reply");
        CheckEq(parse_session_reply(wire, offset).head.status, Status::NotFound, "unknown path returns 404");
        auto wrong_method = parse_session_reply(wire, offset);
        CheckEq(wrong_method.head.status, Status::MethodNotAllowed, "known path returns 405");
        CheckEq(wrong_method.head.headers.Get("Allow"), "POST", "405 includes allowed methods");
        CheckEq(offset, wire.size(), "all chunk pipeline responses consumed");
    }

    /**
     * @brief Verify fatal parse errors preserve one error/post response and do not invoke the handler.
     * @param executor Fixture strand.
     */
    auto session_errors_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        struct Case { std::string wire; Status status; bool half_close{}; };
        std::vector<Case> const cases{
            { "BREW / HTTP/1.1\r\nHost: local\r\n\r\n", Status::NotImplemented },
            { "GET / HTTP/1.2\r\nHost: local\r\n\r\n", Status::HttpVersionNotSupported },
            { "GET /xxxxxxxxxxxxxxxxxxxxxxxxx HTTP/1.1\r\nHost: local\r\n\r\n", Status::UriTooLong },
            { "GET / HTTP/1.1\r\nHost: local\r\nX-Big: " + std::string(150, 'x') + "\r\n\r\n",
                Status::RequestHeaderFieldsTooLarge },
            { "POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\n", Status::ContentTooLarge },
            { "POST / HTTP/1.1\r\nHost: local\r\nExpect: magic\r\nContent-Length: 1\r\n\r\n", Status::ExpectationFailed },
            { "POST / HTTP/1.1\r\nHost: local\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n0\r\n\r\n",
                Status::ContentTooLarge },
            { "POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 3\r\n\r\na", Status::BadRequest, true },
            { "GET / HTTP/1.1\r\nHost:", Status::BadRequest, true },
        };
        for (auto const& test : cases) {
            server::Router router;
            int calls{}, errors{}, posts{};
            Check(router.Post("/", [&](auto&, auto&) { ++calls; }).has_value(), "register error sentinel");
            Check(router.SetErrorHandler([&](auto&, auto&) { ++errors; }).has_value(), "register error hook");
            Check(router.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "register post hook");
            server::ServerOptions options;
            options.uri_bytes = 24;
            options.header_bytes = 100;
            options.body_bytes = 3;
            auto [wire, result] = co_await session_exchange(executor, router, test.wire, options, test.half_close);
            std::size_t offset{};
            auto response = parse_session_reply(wire, offset);
            CheckEq(response.head.status, test.status, "parser status reaches peer");
            CheckEq(response.body, std::format("{} {}\n", static_cast<unsigned>(test.status), ReasonPhrase(test.status)),
                "all protocol errors share the safe default code/reason format");
            CheckEq(response.head.headers.Get("Content-Type"), "text/plain; charset=utf-8", "default error media type is consistent");
            CheckEq(response.head.headers.Get("Connection"), "close", "fatal parse errors close");
            CheckEq(result.responses, std::size_t{ 1 }, "one final parser error response");
            CheckEq(calls, 0, "handler never observes malformed input");
            CheckEq(errors, 1, "error hook executes once");
            CheckEq(posts, 1, "post hook executes once");
            CheckEq(offset, wire.size(), "no second response on fatal error");
        }
    }

    /**
     * @brief Verify early rejection does not wait for a body, even when the client has sent headers only.
     * @param executor Fixture strand.
     */
    auto session_early_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        for (auto method : { "POST", "PUT", "PATCH", "DELETE" }) {
            for (auto length : { "0", "100" }) {
                server::Router router;
                auto [wire, result] = co_await session_exchange(executor, router,
                    std::format("{} /absent HTTP/1.1\r\nHost: local\r\nContent-Length: {}\r\n\r\n", method, length));
                std::size_t offset{};
                auto reply = parse_session_reply(wire, offset);
                CheckEq(reply.head.status, Status::NotFound, "unmatched unsafe method returns 404 before body");
                CheckEq(reply.head.headers.Get("Connection"), "close", "unmatched unsafe method always closes");
                CheckEq(result.responses, std::size_t{ 1 }, "early rejection sends one response");
            }
        }
        server::Router router;
        Check(router.SetPreRoutingHandler([](auto&, auto& response) {
            Check(response.SetError(Status::Forbidden).has_value(), "pre-routing rejects upload");
            return true;
        }).has_value(), "register early response hook");
        auto [wire, result] = co_await session_exchange(executor, router,
            "POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 100\r\n\r\n");
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).head.status, Status::Forbidden, "pre-routing response sent without body");
        CheckEq(result.responses, std::size_t{ 1 }, "early pre-routing response completes");
    }

    /**
     * @brief Verify application exceptions and post failures cannot reuse or emit partial responses.
     * @param executor Fixture strand.
     */
    auto session_exceptions_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        server::Router router;
        Check(router.Get("/", [](auto&, auto&) { throw std::runtime_error{ "private failure" }; }).has_value(),
            "register throwing handler");
        Check(router.SetExceptionHandler([](auto&, auto& response, std::exception_ptr) {
            Check(response.SetStatus(Status::Ok).has_value(), "custom exception status");
            Check(response.Headers().Set("Connection", "keep-alive").has_value(), "attempt persistence after exception");
            Check(response.SetContent("recovered", "text/plain").has_value(), "custom exception content");
        }).has_value(), "register exception transformer");
        auto [wire, result] = co_await session_exchange(executor, router, "GET / HTTP/1.1\r\nHost: local\r\n\r\n");
        std::size_t offset{};
        auto reply = parse_session_reply(wire, offset);
        CheckEq(reply.body, "recovered", "exception transformer reaches wire");
        Check(detail::has_header_token(reply.head.headers, "Connection", "close"),
            "application cannot override fatal close policy");
        Check(result.failure != nullptr, "original application exception retained for logging");
        Check(router.SetPostRoutingHandler([](auto&, auto&) { throw 42; }).has_value(), "register throwing post hook");
        auto post = co_await session_exchange(executor, router, "GET / HTTP/1.1\r\nHost: local\r\n\r\n");
        Check(post.first.empty(), "post failure emits no partial response");
        CheckEq(post.second.responses, std::size_t{}, "post failure writes no response");
    }

    /**
     * @brief Verify idle keep-alive expiry and the configured request-count persistence bound.
     * @param executor Fixture strand.
     */
    auto session_persistence_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        server::Router router;
        Check(router.Get("/", [](auto&, auto& response) {
            Check(response.SetContent("ok", "text/plain").has_value(), "set persistence content");
        }).has_value(), "register persistence route");
        server::ServerOptions options;
        options.keep_alive_timeout = 40ms;
        auto [idle, result] = co_await session_exchange(executor, router, "GET / HTTP/1.1\r\nHost: local\r\n\r\n", options);
        CheckEq(result.error, std::optional{ Error::ReadTimeout }, "idle connection expires without a second response");
        CheckEq(result.responses, std::size_t{ 1 }, "idle timeout preserves completed response count");
        options.keep_alive_max_requests = 1;
        auto bounded = co_await session_exchange(executor, router,
            "GET / HTTP/1.1\r\nHost: local\r\n\r\nGET / HTTP/1.1\r\nHost: local\r\n\r\n", options);
        std::size_t offset{};
        CheckEq(parse_session_reply(bounded.first, offset).head.headers.Get("Connection"), "close",
            "request-count bound forces close");
        CheckEq(bounded.second.responses, std::size_t{ 1 }, "pipelined successor is not dispatched past bound");
    }

    /**
     * @brief Trickle one byte per interval until the peer observes closure or all bytes are sent.
     * @param pair Owned peer connection.
     * @param input Owned incomplete phase bytes.
     * @return The final write result; an early close is expected in deadline tests.
     */
    auto trickle_session_input(
        std::shared_ptr<LoopbackPair> pair,
        std::string input
    ) -> asio::awaitable<Result<void>> {
        asio::steady_timer interval{ co_await asio::this_coro::executor };
        for (char const& byte : input) {
            interval.expires_after(20ms);
            co_await interval.async_wait(asio::use_awaitable);
            auto [error, count] = co_await asio::async_write(pair->peer,
                asio::buffer(&byte, 1), asio::as_tuple(asio::use_awaitable));
            if (error || count != 1) { co_return std::unexpected{ Error::Write }; }
        }
        co_return Result<void>{};
    }

    /**
     * @brief Prove read progress cannot renew the whole-header or whole-body deadline.
     * @param executor Fixture strand.
     */
    auto session_deadlines_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        for (bool body_phase : { false, true }) {
            server::Router router;
            unsigned errors{}, posts{};
            Check(router.Post("/", [](auto&, auto&) { Check(false, "timed-out input cannot dispatch"); }).has_value(),
                "register timeout sentinel");
            Check(router.SetErrorHandler([&errors](auto& request, auto& response) -> Task<void> {
                ++errors;
                CheckEq(response.Status(), Status::RequestTimeout, "timeout enters the ordinary error transformer");
                Check(request.Body().empty(), "partial body is never exposed to response hooks");
                co_await Sleep(1ms);
                Check(response.SetContent("request timed out", "text/plain").has_value(), "rewrite timeout format asynchronously");
            }).has_value(), "customize both timeout phases");
            Check(router.SetPostRoutingHandler([&posts](auto&, auto&) { ++posts; }).has_value(), "observe final timeout response once");
            auto pair = co_await make_loopback(executor);
            server::ServerOptions options;
            options.read_timeout = 100ms;
            Check(detail::RouterAccess::Freeze(router).has_value(), "freeze deadline route");
            auto session = *impl::Session::Create(pair->connection, router, options);
            auto complete = spawn_transport(executor, session->Run());
            auto const started = std::chrono::steady_clock::now();
            std::string const head = "POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 50\r\n\r\n";
            if (body_phase) { co_await asio::async_write(pair->peer, asio::buffer(head), asio::use_awaitable); }
            auto sender = spawn_transport(executor, trickle_session_input(pair,
                body_phase ? std::string(50, 'x') : head));
            auto wire = co_await receive_peer(pair);
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, Status::RequestTimeout, "timed-out request returns 408");
            CheckEq(reply.body, "request timed out", "custom error format reaches the peer after timeout");
            CheckEq(reply.head.headers.Get("Connection"), "close", "timeout response always closes");
            CheckEq(offset, wire.size(), "timeout writes exactly one framed response");
            auto result = co_await complete->Wait();
            auto const elapsed = std::chrono::steady_clock::now() - started;
            Check(elapsed >= 70ms && elapsed < 600ms, "phase deadline is not renewed by individual bytes");
            CheckEq(result.error, std::optional{ Error::ReadTimeout }, "whole phase reports read timeout");
            Check(result.responses == 1 && errors == 1 && posts == 1, "timeout finalizes one error and one post hook");
            pair->peer.close();
            static_cast<void>(co_await sender->Wait());
            Check(detail::RouterAccess::Thaw(router).has_value(), "timeout drains route configuration");
        }
    }

    /**
     * @brief Check lazy root ownership, wrapper destruction, duplicate Run rejection, and idle EOF.
     * @param executor Fixture strand.
     */
    auto session_ownership_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        auto router = std::make_unique<server::Router>();
        auto owned = std::make_shared<int>(7);
        std::weak_ptr<int> handler_lifetime = owned;
        Check(router->Get("/", [owner = std::move(owned)](auto&, auto& response) {
            CheckEq(*owner, 7, "handler capture remains owned after wrapper destruction");
            Check(response.SetContent("owned", "text/plain").has_value(), "owned route response");
        }).has_value(), "register owned handler");
        Check(detail::RouterAccess::Freeze(*router).has_value(), "freeze owned route");
        auto session = *impl::Session::Create(pair->connection, *router);
        auto pending = session->Run();
        std::weak_ptr<impl::Session> lifetime = session;
        session.reset();
        router.reset();
        Check(!lifetime.expired() && !handler_lifetime.expired(), "unstarted Run retains session and callbacks");
        auto complete = spawn_transport(executor, std::move(pending));
        std::string const input = "GET / HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n";
        co_await asio::async_write(pair->peer, asio::buffer(input), asio::use_awaitable);
        auto wire = co_await receive_peer(pair);
        CheckEq((co_await complete->Wait()).responses, std::size_t{ 1 }, "owned root completes");
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "owned", "owned callback is callable");
        Check(lifetime.expired() && handler_lifetime.expired(), "root completion releases session and router");

        pair = co_await make_loopback(executor);
        server::Router idle_router;
        session = *impl::Session::Create(pair->connection, idle_router);
        auto idle = spawn_transport(executor, session->Run());
        auto duplicate = spawn_transport(executor, session->Run());
        pair->peer.shutdown(asio::socket_base::shutdown_send);
        Check((co_await receive_peer(pair)).empty(), "idle EOF does not generate an unsolicited 400");
        auto first = co_await idle->Wait();
        auto second = co_await duplicate->Wait();
        Check((first.error == Error::Busy) != (second.error == Error::Busy),
            "exactly one duplicate Run is rejected without closing the other loop");
        CheckEq(first.responses + second.responses, std::size_t{}, "idle EOF writes no response");
    }

    /**
     * @brief Wait for 100 before transmitting the body, then reuse the connection for a successor.
     * @param executor Fixture strand.
     */
    auto session_continue_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        server::Router router;
        int handlers{}, posts{};
        Check(router.Post("/", [&](auto& request, auto& response) {
            ++handlers;
            CheckEq(request.Body(), "body", "Expect body is complete before dispatch");
            Check(response.SetContent("accepted", "text/plain").has_value(), "accepted upload");
        }).has_value(), "register Expect route");
        Check(router.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "register Expect post hook");
        auto pair = co_await make_loopback(executor, { 5, 16 * 1024 * 1024 });
        Check(detail::RouterAccess::Freeze(router).has_value(), "freeze Expect configuration");
        server::ServerOptions options;
        options.keep_alive_max_requests = 2;
        auto session = *impl::Session::Create(pair->connection, router, options);
        auto complete = spawn_transport(executor, session->Run());
        std::string const head = "POST / HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\nExpect: 100-CoNtInUe\r\n\r\n";
        co_await asio::async_write(pair->peer, asio::buffer(head), asio::use_awaitable);
        std::string interim(25, '\0');
        co_await asio::async_read(pair->peer, asio::buffer(interim), asio::use_awaitable);
        CheckEq(interim, "HTTP/1.1 100 Continue\r\n\r\n", "100 arrives before any body is sent");
        std::string const body_and_next = "bodyPOST / HTTP/1.1\r\nHost: local\r\nContent-Length: 4\r\n\r\nbody";
        co_await asio::async_write(pair->peer, asio::buffer(body_and_next), asio::use_awaitable);
        auto wire = co_await receive_peer(pair);
        auto result = co_await complete->Wait();
        std::size_t offset{};
        CheckEq(parse_session_reply(wire, offset).body, "accepted", "first final response after 100");
        CheckEq(parse_session_reply(wire, offset).body, "accepted", "connection reusable after accepted Expect");
        CheckEq(result.responses, std::size_t{ 2 }, "100 excluded from final response count and request limit");
        CheckEq(handlers, 2, "both complete uploads dispatched");
        CheckEq(posts, 2, "post runs only for final responses");
        Check(detail::RouterAccess::Thaw(router).has_value(), "Expect route drains");
    }

    /**
     * @brief Verify early final responses never wait for an Expect body or emit an interim response.
     * @param executor Fixture strand.
     */
    auto session_expect_rejection_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        struct Case { std::string wire; Status status; };
        std::vector<Case> const cases{
            { "POST /absent HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n",
                Status::NotFound },
            { "GET /absent HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n",
                Status::NotFound },
            { "PUT / HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n",
                Status::MethodNotAllowed },
            { "POST / HTTP/1.1\r\nHost: local\r\nExpect: magic\r\nContent-Length: 3\r\n\r\n",
                Status::ExpectationFailed },
            { "POST / HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\nContent-Length: 9\r\n\r\n",
                Status::ContentTooLarge },
        };
        for (auto const& test : cases) {
            server::Router router;
            Check(router.Post("/", [](auto&, auto&) { Check(false, "rejected Expect cannot dispatch"); }).has_value(),
                "register rejected Expect sentinel");
            server::ServerOptions options;
            options.body_bytes = 3;
            auto [wire, result] = co_await session_exchange(executor, router, test.wire, options);
            Check(!wire.contains("100 Continue"), "early final response contains no 100");
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            CheckEq(reply.head.status, test.status, "Expect rejection status");
            CheckEq(reply.head.headers.Get("Connection"), "close", "early Expect final closes connection");
            CheckEq(result.responses, std::size_t{ 1 }, "one final Expect rejection");
        }
        server::Router router;
        Check(router.Post("/", [](auto& request, auto& response) {
            CheckEq(request.Body(), "abc", "HTTP/1.0 reads body without an interim response");
            Check(response.SetContent("ignored", "text/plain").has_value(), "HTTP/1.0 response");
        }).has_value(), "register HTTP/1.0 upload");
        auto ignored = co_await session_exchange(executor, router,
            "POST / HTTP/1.0\r\nExpect: magic\r\nContent-Length: 3\r\n\r\nabc");
        Check(!ignored.first.contains("100 Continue"), "HTTP/1.0 ignores Expect");
        std::size_t offset{};
        CheckEq(parse_session_reply(ignored.first, offset).head.status, Status::Ok, "HTTP/1.0 upload succeeds");
        Check(router.SetPreRoutingHandler([](auto&, auto& response) {
            Check(response.SetError(Status::Forbidden).has_value(), "reject expected upload before routing");
            return true;
        }).has_value(), "register Expect interception");
        auto intercepted = co_await session_exchange(executor, router,
            "POST / HTTP/1.1\r\nHost: local\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\n");
        Check(!intercepted.first.contains("100 Continue"), "pre-routing interception emits no 100");
        offset = 0;
        CheckEq(parse_session_reply(intercepted.first, offset).head.status, Status::Forbidden, "pre-routing Expect rejection");
    }

    /**
     * @brief Cancel an awaited user Task from another thread and drain its request/route owners.
     * @param executor Fixture strand.
     * @param session Published before signaling entered to the canceling thread.
     * @param entered Cross-thread handoff after the handler begins.
     */
    auto session_cancel_scenario(
        asio::any_io_executor executor,
        std::shared_ptr<impl::Session>& session,
        std::latch& entered
    ) -> asio::awaitable<void> {
        server::Router router;
        std::weak_ptr<int> released;
        Check(router.Get("/", [&](auto&, auto&) -> Task<void> {
            auto owner = std::make_shared<int>(7);
            released = owner;
            entered.count_down();
            co_await Sleep(24h);
            Check(false, "canceled handler cannot reach code after Sleep");
        }).has_value(), "register suspended handler");
        auto pair = co_await make_loopback(executor);
        Check(detail::RouterAccess::Freeze(router).has_value(), "freeze cancellation route");
        session = *impl::Session::Create(pair->connection, router);
        auto complete = spawn_transport(executor, session->Run());
        std::string const input = "GET / HTTP/1.1\r\nHost: local\r\n\r\n";
        co_await asio::async_write(pair->peer, asio::buffer(input), asio::use_awaitable);
        Check((co_await receive_peer(pair)).empty(), "cancellation writes no 500");
        auto result = co_await complete->Wait();
        CheckEq(result.error, std::optional{ Error::Canceled }, "session reports cancellation");
        Check(released.expired(), "handler-local owner is destroyed before session completion");
        Check(detail::RouterAccess::Thaw(router).has_value(), "canceled route lease fully drains");
    }

    /**
     * @brief Complete one admitted request after drain while rejecting a fully buffered pipelined successor.
     * @param executor Independent fixture strand.
     */
    auto session_drain_pipeline_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        auto pair = co_await make_loopback(executor);
        server::Router router;
        std::shared_ptr<impl::Session> session;
        unsigned calls{};
        Check(router.Get("/", [&](auto&, auto& response) -> Task<void> {
            ++calls;
            co_await impl::ToTask(pair->connection->Executor(), session->Drain());
            co_await Sleep(2ms);
            Check(response.SetContent("completed", "text/plain").has_value(), "draining handler remains uncanceled");
        }).has_value(), "register draining pipeline route");
        Check(detail::RouterAccess::Freeze(router).has_value(), "freeze draining pipeline route");
        session = *impl::Session::Create(pair->connection, router);
        auto complete = spawn_transport(executor, session->Run());
        std::string const request = "GET / HTTP/1.1\r\nHost: local\r\n\r\n";
        auto input = request + request;
        co_await asio::async_write(pair->peer, asio::buffer(input), asio::use_awaitable);
        auto wire = co_await receive_peer(pair);
        auto result = co_await complete->Wait();
        std::size_t offset{};
        auto response = parse_session_reply(wire, offset);
        Check(response.head.status == Status::Ok && response.body == "completed", "admitted response completes during drain");
        CheckEq(response.head.headers.Get("Connection"), std::string_view{ "close" }, "draining response closes persistence");
        Check(offset == wire.size() && calls == 1 && result.responses == 1, "buffered successor never reaches middleware or handler");
        Check(detail::RouterAccess::Thaw(router).has_value(), "drained route lease released");
    }

    /**
     * @brief Retire an idle or not-yet-started session without waiting for its ordinary header timeout.
     * @param executor Independent fixture strand.
     */
    auto session_idle_drain_scenario(asio::any_io_executor executor) -> asio::awaitable<void> {
        server::Router router;
        auto pair = co_await make_loopback(executor);
        Check(detail::RouterAccess::Freeze(router).has_value(), "freeze idle drain router");
        auto session = *impl::Session::Create(pair->connection, router);
        auto complete = spawn_transport(executor, session->Run());
        co_await session->Drain();
        Check((co_await receive_peer(pair)).empty(), "idle drain emits no synthetic error response");
        auto result = co_await complete->Wait();
        CheckEq(result.responses, std::size_t{}, "idle drain completes without requests");
        Check(detail::RouterAccess::Thaw(router).has_value(), "idle drain releases session ownership");
    }

    /**
     * @brief Run one session scenario with a fresh multithreaded runtime.
     * @param scenario Factory borrowing only the runtime executor.
     */
    auto run_session_scenario(asio::awaitable<void> (*scenario)(asio::any_io_executor)) -> void {
        TransportRuntime runtime;
        runtime.Run(scenario(runtime.Executor()));
    }

    /**
     * @brief Run cross-thread cancellation, joining the canceling thread outside I/O execution.
     */
    auto session_cancellation() -> void {
        TransportRuntime runtime;
        std::latch entered{ 1 };
        std::shared_ptr<impl::Session> session;
        std::jthread cancel{ [&] { entered.wait(); session->Stop(); } };
        runtime.Run(session_cancel_scenario(runtime.Executor(), session, entered));
    }

} // namespace mhttp::test

export namespace mhttp::test {

    /**
     * @brief Run named private session fixtures exported only by the isolated verification project.
     * @param argc Argument count including program name.
     * @param argv Optional case-name filters.
     * @return Test process exit status.
     */
    auto RunSessionTests(int argc, char* argv[]) -> int {
        return RunTests(argc, argv, {
            { "session pipeline", [] { run_session_scenario(session_pipeline_scenario); } },
            { "session draining pipeline", [] { run_session_scenario(session_drain_pipeline_scenario); } },
            { "session idle drain", [] { run_session_scenario(session_idle_drain_scenario); } },
            { "session chunked", [] { run_session_scenario(session_chunked_scenario); } },
            { "session parser errors", [] { run_session_scenario(session_errors_scenario); } },
            { "session early rejection", [] { run_session_scenario(session_early_scenario); } },
            { "session exceptions", [] { run_session_scenario(session_exceptions_scenario); } },
            { "session persistence", [] { run_session_scenario(session_persistence_scenario); } },
            { "session phase deadlines", [] { run_session_scenario(session_deadlines_scenario); } },
            { "session ownership", [] { run_session_scenario(session_ownership_scenario); } },
            { "session 100 continue", [] { run_session_scenario(session_continue_scenario); } },
            { "session Expect rejection", [] { run_session_scenario(session_expect_rejection_scenario); } },
            { "session cancellation", session_cancellation },
        });
    }

} // namespace mhttp::test
