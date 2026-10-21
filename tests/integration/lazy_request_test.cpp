/**
 * @file lazy_request_test.cpp
 * @brief Verify bounded owned request chunks, retired read Tasks, error hooks, and lazy route replacement.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Start a lazy-request listener using a dynamic loopback port.
 * @param server Owned by the test, with a running longer-lived runtime.
 * @return Bound port, or an exception on fixture failure.
 */
auto start_lazy_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start lazy listener" }; }
    return *port;
}

/**
 * @brief Stream binary bodies in bounded owned chunks and preserve ordinary buffering and connection reuse.
 */
auto lazy_binary_persistence() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start lazy runtime");
    mhttp::server::HttpServer server{ runtime, { .body_bytes = 8192, .request_chunk_bytes = 7 } };
    Check(server.RouteLazy(mhttp::Method::Post, "/lazy/:id", [owned = std::make_unique<unsigned>(7)](
        auto& request, auto& response
    ) -> mhttp::Task<void> {
        CheckEq(request.template PathParam<unsigned>("id"), std::optional{ 42u }, "lazy path captures exist before intake");
        Check(request.Body().empty() && request.Trailers().Empty(), "lazy body has no aggregate storage");
        std::string body;
        std::optional<std::string> retained;
        for (;;) {
            auto chunk = co_await request.ReadBody();
            Check(chunk.has_value(), "read bounded binary chunk");
            if (!*chunk) { break; }
            Check(!(**chunk).empty() && (**chunk).size() <= *owned, "each owned chunk respects configured bound");
            if (!retained) { retained = **chunk; }
            body += **chunk;
            co_await mhttp::Sleep(1ms);
        }
        Check(body.starts_with(retained.value_or("")), "retained chunks survive subsequent socket reads");
        auto end = co_await request.ReadBody();
        Check(end && !*end && request.Body().empty(), "repeated end is stable without aggregation");
        Check(response.SetContent(std::move(body), "application/octet-stream").has_value(), "echo streamed binary content");
    }).has_value(), "register move-only lazy route");
    Check(server.Post("/buffered", [](auto& request, auto& response) -> mhttp::Task<void> {
        auto read = co_await request.ReadBody();
        Check(!read && read.error() == mhttp::Error::InvalidRequest, "buffered routes have no reader");
        Check(response.SetContent(request.Body(), "application/octet-stream").has_value(), "ordinary route retains buffered body");
    }).has_value(), "register buffered route beside lazy route");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_lazy_server(server) };
    std::string body(257, '\0');
    for (std::size_t index{}; index != body.size(); ++index) { body[index] = static_cast<char>(index); }
    std::optional<mhttp::Endpoint> endpoint;
    for (auto const& path : { "/lazy/42", "/buffered", "/lazy/42" }) {
        auto reply = mhttp::SyncWait(runtime, client.Post(path, body, "application/octet-stream"));
        Check(reply && reply->Body() == body, "binary data survives lazy and buffered intake");
        if (endpoint) { CheckEq(reply->LocalEndpoint(), *endpoint, "complete intake permits persistence"); }
        endpoint = reply->LocalEndpoint();
    }
    auto empty = mhttp::SyncWait(runtime, client.Post("/lazy/42", ""));
    Check(empty && empty->Body().empty() && empty->LocalEndpoint() == endpoint, "empty framing permits reuse");
    Check(server.Stop().has_value(), "drain binary lazy requests");
}

/**
 * @brief Preserve an ignored read timeout through the error/post pipeline and reject late read Tasks.
 */
auto lazy_failure_and_retirement() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start retired-read runtime");
    std::optional<mhttp::Task<mhttp::Result<std::optional<std::string>>>> late;
    std::latch accessed{ 1 };
    std::atomic<unsigned> errors{}, posts{};
    mhttp::server::HttpServer server{ runtime, { .read_timeout = 50ms } };
    Check(server.RouteLazy(mhttp::Method::Post, "/expire", [&](auto& request, auto& response) -> mhttp::Task<void> {
        late.emplace(request.ReadBody());
        co_await mhttp::Sleep(100ms);
        auto expired = co_await request.ReadBody();
        Check(!expired && expired.error() == mhttp::Error::ReadTimeout, "handler work does not reset intake deadline");
        Check(response.SetContent("ignored error", "text/plain").has_value(), "handler attempts to ignore failed intake");
    }).has_value(), "register delayed reader");
    Check(server.SetErrorHandler([&](auto& request, auto& response) -> mhttp::Task<void> {
        ++errors;
        auto read = co_await request.ReadBody();
        Check(!read && read.error() == mhttp::Error::InvalidRequest, "error hook cannot resume retired intake");
        CheckEq(response.Status(), mhttp::Status::RequestTimeout, "sticky timeout overrides ignored success");
        Check(response.SetContent("timeout", "text/plain").has_value(), "customize timeout content");
    }).has_value(), "install lazy error hook");
    Check(server.SetPostRoutingHandler([&](auto&, auto&) { ++posts; }).has_value(), "install lazy post hook");
    Check(server.SetAccessLogger([&](auto const&, auto const&, auto) { accessed.count_down(); }).has_value(), "publish retirement");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_lazy_server(server) };
    auto reply = mhttp::SyncWait(runtime, client.Post("/expire", "unread"));
    Check(reply && reply->Status() == mhttp::Status::RequestTimeout && reply->Body() == "timeout", "one finalized timeout response");
    CheckEq(reply->Header("Connection"), std::string_view{ "close" }, "failed lazy intake closes");
    accessed.wait();
    auto rejected = mhttp::SyncWait(runtime, std::move(*late));
    Check(!rejected && rejected.error() == mhttp::Error::InvalidRequest, "unstarted read cannot access retired request");
    late.reset();
    Check(server.Stop().has_value() && errors == 1 && posts == 1, "error and post hooks run exactly once");
}

/**
 * @brief Keep body admission policy attached to replaced route bindings and reject standalone body reads.
 */
auto lazy_registration() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start lazy metadata runtime");
    mhttp::server::Router router;
    auto handler = [](auto&, auto&) {};
    for (bool lazy : { true, false, true }) {
        auto registered = lazy ? router.RouteLazy(mhttp::Method::Post, "/item", handler) : router.Post("/item", handler);
        Check(registered.has_value(), "replace intake policy with binding");
        auto request = mhttp::detail::ServerRequestAccess::Create({ .method = "POST", .target = "/item" });
        mhttp::server::Response response;
        auto route = mhttp::SyncWait(runtime, mhttp::detail::RouterAccess::Prepare(router, request, response));
        Check(route.handler && route.lazy == lazy, "selected callback retains its matching intake policy");
        auto read = mhttp::SyncWait(runtime, request.ReadBody());
        Check(!read && read.error() == mhttp::Error::InvalidRequest, "standalone preparation never invents body input");
        auto busy = router.RouteLazy(mhttp::Method::Post, "/other", handler);
        Check(!busy && busy.error() == mhttp::Error::Busy, "lazy routes retain existing configuration lease guard");
    }
    Check(!mhttp::detail::validate_server_options({ .request_chunk_bytes = 0 }), "zero read chunk bound is rejected");
}

/**
 * @brief Retire completed intake before a deferred response provider and reject old reads during socket reuse.
 */
auto lazy_response_and_reuse() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start composable lazy runtime");
    std::optional<mhttp::Task<mhttp::Result<std::optional<std::string>>>> late;
    std::latch next_entered{ 1 };
    mhttp::server::HttpServer server{ runtime, { .read_timeout = 50ms, .request_chunk_bytes = 3 } };
    Check(server.RouteLazy(mhttp::Method::Post, "/echo", [&](auto& request, auto& response) -> mhttp::Task<void> {
        std::string body;
        for (;;) {
            auto chunk = co_await request.ReadBody();
            Check(chunk.has_value(), "read before installing response provider");
            if (!*chunk) { break; }
            body += **chunk;
        }
        late.emplace(request.ReadBody());
        Check(response.SetChunkedContentProvider([&request, body = std::move(body)](
            auto& writer
        ) -> mhttp::Task<mhttp::Result<void>> {
            auto retired = co_await request.ReadBody();
            Check(!retired && retired.error() == mhttp::Error::InvalidRequest, "provider cannot reopen request intake");
            co_return co_await writer.Write(body);
        }).has_value(), "defer owned streamed echo after complete upload");
    }).has_value(), "combine lazy upload and streamed response");
    Check(server.Get("/next", [&](auto&, auto& response) -> mhttp::Task<void> {
        next_entered.count_down();
        co_await mhttp::Sleep(100ms);
        Check(response.SetContent("next", "text/plain").has_value(), "complete successor after old intake deadline");
    }).has_value(), "register delayed successor");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_lazy_server(server) };
    auto echo = mhttp::SyncWait(runtime, client.Post("/echo", "echo"));
    Check(echo && echo->Body() == "echo", "lazy intake composes with chunked output");
    mhttp::Result<mhttp::client::Response> next = std::unexpected{ mhttp::Error::Unknown };
    std::jthread caller{ [&] { next = mhttp::SyncWait(runtime, client.Get("/next")); } };
    next_entered.wait();
    auto retired = mhttp::SyncWait(runtime, std::move(*late));
    Check(!retired && retired.error() == mhttp::Error::InvalidRequest, "old reader cannot consume an active successor");
    late.reset();
    caller.join();
    Check(next && next->Body() == "next" && next->LocalEndpoint() == echo->LocalEndpoint(), "retired reads preserve socket reuse");
    Check(server.Stop().has_value(), "drain composable lazy requests");
}

/**
 * @brief Recognize an already consumed fixed body after application work exceeds the intake deadline.
 */
auto lazy_completed_body_deadline() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start completed-intake runtime");
    mhttp::server::HttpServer server{ runtime, { .read_timeout = 100ms, .request_chunk_bytes = 3 } };
    Check(server.RouteLazy(mhttp::Method::Post, "/read", [](auto& request, auto& response) -> mhttp::Task<void> {
        std::string body;
        while (body.size() != 5) {
            auto chunk = co_await request.ReadBody();
            Check(chunk && *chunk, "consume the complete declared body before its deadline");
            body += **chunk;
        }
        co_await mhttp::Sleep(200ms);
        auto end = co_await request.ReadBody();
        Check(end && !*end, "end of an already consumed body requires no timed read");
        Check(response.SetContent(std::move(body), "text/plain").has_value(), "reply after application work");
    }).has_value(), "register completed-body reader");
    Check(server.Get("/next", [](auto&, auto& response) {
        Check(response.SetContent("next", "text/plain").has_value(), "reply on reused connection");
    }).has_value(), "register completed-body successor");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_lazy_server(server) };
    auto reply = mhttp::SyncWait(runtime, client.Post("/read", "ready"));
    Check(reply && reply->Status() == mhttp::Status::Ok && reply->Body() == "ready", "application work after intake cannot cause 408");
    auto next = mhttp::SyncWait(runtime, client.Get("/next"));
    Check(next && next->LocalEndpoint() == reply->LocalEndpoint(), "completed body remains reusable after intake deadline");
    Check(server.Stop().has_value(), "drain completed-intake requests");
}

/**
 * @brief Run bounded public lazy-body scenarios.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Process status.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "lazy binary persistence", lazy_binary_persistence },
        { "lazy failure and retirement", lazy_failure_and_retirement },
        { "lazy registration policy", lazy_registration },
        { "lazy response and reused input", lazy_response_and_reuse },
        { "lazy completed body deadline", lazy_completed_body_deadline },
    });
}
