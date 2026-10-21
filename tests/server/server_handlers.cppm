/**
 * @file server_handlers.cppm
 * @brief Verify handler composition and concurrent exception transformation through complete HTTP exchanges.
 */
module mhttp:test.server_handlers;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;
import :test.server_peer;

namespace mhttp::test {

    /**
     * @brief Record aspect phases in response-owned fields without sharing mutable state between requests.
     */
    struct WireTraceAspect {
        std::string name; ///< Owned phase prefix.
        bool check_id{}; ///< Require captures to be populated before Before.
        bool reject{}; ///< Return false after writing a Forbidden response.

        /**
         * @brief Record entry, inspect captures, and optionally short-circuit the handler chain.
         * @param request Borrowed matched request.
         * @param response Borrowed response whose fields retain phase ordering.
         * @return Whether to continue the remaining Before members and handler.
         */
        auto Before(server::Request& request, server::Response& response) -> bool {
            Check(response.Headers().Add("X-Trace", name + ".before").has_value(), "trace Before on the wire");
            if (check_id) { CheckEq(request.PathParam<int>("id"), std::optional{ 42 }, "Before sees typed path capture"); }
            if (reject) { Check(response.SetError(Status::Forbidden).has_value(), "Before prepares preserved 403"); }
            return !reject;
        }

        /**
         * @brief Suspend before recording exit to verify asynchronous aspect sequencing.
         * @param request Unused borrowed request.
         * @param response Borrowed until the entire routing task completes.
         * @return True to continue remaining After members.
         */
        auto After(server::Request& request, server::Response& response) -> Task<bool> {
            co_await Sleep(1ms);
            Check(response.Headers().Add("X-Trace", name + ".after").has_value(), "trace asynchronous After on the wire");
            co_return true;
        }
    };

    /**
     * @brief Check complete responses after interception, aspect execution, and custom missing-route handling.
     * @param executor Fixture strand.
     * @param endpoint Owned dynamic loopback endpoint.
     */
    auto server_handler_order_scenario(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        for (auto path : { "/allow/42", "/deny/42", "/absent", "/intercept" }) {
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET {} HTTP/1.1\r\nHost: local\r\nConnection: close\r\n\r\n", path));
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            std::vector<std::string_view> trace{ "pre" };
            auto const target = std::string_view{ path };
            Status status{ Status::Ok };
            std::string body{ "intercepted" };
            if (target != "/intercept") {
                trace.insert(trace.end(), { "global1.before", "global2.before" });
                if (target == "/absent") {
                    status = Status::NotFound;
                    body = "/absent";
                    trace.insert(trace.end(), { "missing.before", "missing", "missing.after" });
                } else {
                    trace.push_back("route1.before");
                    if (target == "/deny/42") { status = Status::Forbidden; body = "403 Forbidden\n"; }
                    else { trace.insert(trace.end(), { "route2.before", "handler" }); body = "allowed"; }
                    trace.insert(trace.end(), { "route1.after", "route2.after" });
                }
                trace.insert(trace.end(), { "global1.after", "global2.after" });
            }
            trace.push_back("post");
            CheckEq(reply.head.status, status, "handler composition status");
            CheckEq(reply.body, body, "short-circuit or composed response survives serialization");
            CheckEq(reply.head.headers.Values("X-Trace"), trace, "global/route phases run in declared order");
            CheckEq(reply.head.headers.Values("X-Post"), std::vector<std::string_view>{ "once" }, "one post hook per final response");
            CheckEq(reply.head.headers.Get("Content-Length"), std::to_string(body.size()), "finalized handler body determines length");
            CheckEq(offset, wire.size(), "one complete response per handled request");
        }
    }

    /**
     * @brief Verify global and route aspects, early interception, typed captures, and custom 404 composition.
     */
    auto server_handler_order() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start handler ordering runtime");
        server::HttpServer server{ runtime };
        Check(server.Use(WireTraceAspect{ "global1" }).has_value(), "register first global aspect");
        Check(server.Use(WireTraceAspect{ "global2" }).has_value(), "register second global aspect");
        Check(server.Get("/allow/:id", [](auto&, auto& response) -> Task<void> {
            co_await Sleep(1ms);
            Check(response.Headers().Add("X-Trace", "handler").has_value(), "record asynchronous handler");
            Check(response.SetContent("allowed", "text/plain").has_value(), "allowed response");
        }, WireTraceAspect{ "route1", true }, WireTraceAspect{ "route2", true }).has_value(), "register ordered route aspects");
        Check(server.Get("/deny/:id", [](auto&, auto&) { Check(false, "denied handler must not run"); },
            WireTraceAspect{ "route1", true, true }, WireTraceAspect{ "route2", true }).has_value(), "register rejecting Before");
        Check(server.Get("/intercept", [](auto&, auto&) { Check(false, "intercepted route must not run"); }).has_value(),
            "register intercepted route sentinel");
        Check(server.SetNotFoundHandler([](auto& request, auto& response) {
            Check(response.Headers().Add("X-Trace", "missing").has_value(), "trace custom 404 handler");
            Check(response.SetContent(std::string{ request.Path() }, "text/plain").has_value(), "custom 404 sees requested path");
        }, WireTraceAspect{ "missing" }).has_value(), "register custom 404 with aspect");
        Check(server.SetPreRoutingHandler([](auto& request, auto& response) -> Task<bool> {
            Check(request.PathParam("id").empty(), "pre-routing precedes capture population");
            Check(response.Headers().Add("X-Trace", "pre").has_value(), "record pre-routing once");
            co_await Sleep(1ms);
            if (request.Path() != "/intercept") { co_return false; }
            Check(response.SetContent("intercepted", "text/plain").has_value(), "interception response");
            co_return true;
        }).has_value(), "register asynchronous interception");
        Check(server.SetPostRoutingHandler([](auto&, auto& response) {
            Check(response.Headers().Add("X-Trace", "post").has_value(), "record final post-routing phase");
            Check(response.Headers().Add("X-Post", "once").has_value(), "append post marker without replacing duplicates");
        }).has_value(), "register final response hook");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind handler ordering listener");
        Check(server.Start().has_value(), "start handler ordering listener");
        TransportRuntime peers;
        peers.Run(server_handler_order_scenario(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain ordered handler frames");
    }

    /**
     * @brief Count simultaneous exception handler frames and release activity on every exit path.
     */
    struct ExceptionActivity {
        std::atomic<unsigned>& active; ///< Counter alive through listener drain.

        /**
         * @brief Publish current activity and update the observed concurrency peak.
         * @param count Current activity counter.
         * @param peak Maximum activity counter.
         */
        ExceptionActivity(std::atomic<unsigned>& count, std::atomic<unsigned>& peak) : active{ count } {
            auto const current = ++active;
            auto observed = peak.load();
            while (observed < current && !peak.compare_exchange_weak(observed, current)) {}
        }

        /**
         * @brief Publish coroutine frame cleanup.
         */
        ~ExceptionActivity() { --active; }
    };

    /**
     * @brief Send one client's sequence of independent failing requests and verify transformed responses.
     * @param executor Fixture strand.
     * @param endpoint Owned listener endpoint.
     * @param client Client identity, determining nonoverlapping request identifiers.
     * @return Number of fully validated responses.
     */
    auto server_exception_client(
        asio::any_io_executor executor,
        Endpoint endpoint,
        unsigned client
    ) -> asio::awaitable<unsigned> {
        for (unsigned index{}; index < 100; ++index) {
            auto const id = client * 100 + index;
            auto const kind = (client + index) % 2 == 0 ? "standard" : "unknown";
            auto wire = co_await server_exchange(executor, endpoint,
                std::format("GET /fail/{}/{} HTTP/1.1\r\nHost: local\r\n\r\n", kind, id));
            std::size_t offset{};
            auto reply = parse_session_reply(wire, offset);
            auto const body = std::format("final:{}", id);
            CheckEq(reply.head.status, Status::Created, "error handler may turn an exception response into success");
            CheckEq(reply.body, body, "concurrent callbacks retain the correct request identity");
            CheckEq(reply.head.headers.Get("Content-Length"), std::to_string(body.size()), "error transformation recomputes wire length");
            CheckEq(reply.head.headers.Get("Connection"), "close", "contained exceptions still require connection close");
            CheckEq(reply.head.headers.Values("X-Post"), std::vector<std::string_view>{ "once" }, "one post hook after error rewriting");
            Check(!wire.contains("Injected") && !wire.contains("private failure"), "private exception details never reach the wire");
            CheckEq(offset, wire.size(), "no duplicate error response");
        }
        co_return 100;
    }

    /**
     * @brief Run ten clients concurrently, each issuing one hundred failing HTTP exchanges.
     * @param executor Fixture strand independent of the listener's Runtime.
     * @param endpoint Owned listener endpoint.
     */
    auto server_exception_clients(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<void> {
        std::vector<std::shared_ptr<TransportResult<unsigned>>> clients;
        for (unsigned index{}; index < 10; ++index) {
            clients.push_back(spawn_transport(executor, server_exception_client(executor, endpoint, index)));
        }
        unsigned responses{};
        for (auto const& client : clients) { responses += co_await client->Wait(); }
        CheckEq(responses, 1000U, "all ten clients complete their one hundred exchanges");
    }

    /**
     * @brief Verify concurrent exception delivery, error transformation, post-routing, and handler cleanup.
     */
    auto server_exception_concurrency() -> void {
        Runtime runtime;
        Check(runtime.Start(4).has_value(), "start concurrent exception runtime");
        std::atomic<unsigned> standard{}, unknown{}, errors{}, posts{}, active{}, peak{}, initial{};
        server::HttpServer server{ runtime };
        Check(server.Get("/fail/:kind/:id", [](auto& request, auto&) {
            if (request.PathParam("kind") == "standard") { throw std::runtime_error{ "private failure\r\nInjected: yes" }; }
            throw 42;
        }).has_value(), "register concurrent failing handler");
        Check(server.SetExceptionHandler([&](auto& request, auto& response, std::exception_ptr failure) -> Task<void> {
            ExceptionActivity activity{ active, peak };
            try { std::rethrow_exception(failure); }
            catch (std::runtime_error const& error) {
                ++standard;
                CheckEq(request.PathParam("kind"), "standard", "standard exception belongs to this request");
                CheckEq(std::string_view{ error.what() }, "private failure\r\nInjected: yes", "custom handler receives original exception");
            } catch (int value) {
                ++unknown;
                CheckEq(value, 42, "custom handler receives original arbitrary exception");
                CheckEq(request.PathParam("kind"), "unknown", "arbitrary exception belongs to this request");
            }
            if (auto id = request.template PathParam<unsigned>("id"); id && *id % 100 == 0) {
                ++initial;
                while (initial < 10) { co_await Sleep(1ms); }
            }
            co_await Sleep(1ms);
            Check(response.SetStatus(Status::BadGateway).has_value(), "customize exception status before error handler");
            Check(response.SetContent("intermediate", "text/plain").has_value(), "custom exception representation");
        }).has_value(), "register concurrent coroutine exception transformer");
        Check(server.SetErrorHandler([&](auto& request, auto& response) {
            ++errors;
            CheckEq(response.Status(), Status::BadGateway, "error handler sees exception handler output exactly once");
            Check(response.SetStatus(Status::Created).has_value(), "replace error with success without recursive transformation");
            Check(response.SetContent(std::format("final:{}", request.PathParam("id")), "text/plain").has_value(),
                "replace body before serialization");
        }).has_value(), "register concurrent error transformer");
        Check(server.SetPostRoutingHandler([&](auto&, auto& response) {
            ++posts;
            Check(response.Headers().Add("X-Post", "once").has_value(), "post marker for rewritten exception response");
        }).has_value(), "register concurrent post hook");
        Check(server.BindToAnyPort("127.0.0.1").has_value(), "bind concurrent exception listener");
        Check(server.Start().has_value(), "start concurrent exception listener");
        TransportRuntime peers;
        peers.Run(server_exception_clients(peers.Executor(), *server.LocalEndpoint()));
        Check(server.Stop().has_value(), "drain every exception callback before inspecting counters");
        Check(standard == 500 && unknown == 500 && errors == 1000 && posts == 1000, "one exception, error, and post callback per request");
        Check(initial == 10 && active == 0 && peak >= 10, "all clients overlap in exception handlers and every frame drains");
    }

} // namespace mhttp::test
