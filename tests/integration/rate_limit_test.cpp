/**
 * @file rate_limit_test.cpp
 * @brief Verify shared HTTP quotas, trusted client identities, lazy upload rejection, and concurrent sessions.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/thread_task.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Start a configured quota listener on a dynamic loopback port.
 * @param server Retained listener.
 * @return Bound port.
 */
auto start_rate_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "rate fixture start failed" }; }
    return *port;
}

/**
 * @brief Share quotas across copied route aspects and distinct sockets, preserving CORS on a denial response.
 */
auto shared_http_routes() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start HTTP quota runtime");
    auto limiter = *mhttp::server::RateLimit::Create({ .max_requests = 2, .window = 60s });
    auto independent = *mhttp::server::RateLimit::Create({ .max_requests = 1, .window = 60s });
    auto cors = *mhttp::server::Cors::Create();
    mhttp::server::HttpServer server{ runtime };
    Check(server.Use(cors).has_value(), "install CORS before route limiter");
    std::atomic<unsigned> calls{};
    auto handler = [&calls](auto&, auto& response) {
        ++calls;
        static_cast<void>(response.SetContent("accepted", "text/plain"));
    };
    Check(server.Get("/a", handler, limiter).has_value() && server.Get("/b", handler, limiter).has_value(), "copy one quota into both routes");
    Check(server.Get("/independent", handler, independent).has_value(), "register independent quota");
    auto port = start_rate_server(server);
    mhttp::client::HttpClient first{ runtime, "127.0.0.1", port };
    mhttp::client::HttpClient second{ runtime, "127.0.0.1", port };
    mhttp::Headers headers;
    Check(headers.Set("Origin", "https://example.com").has_value(), "quota response origin");
    auto a = mhttp::SyncWait(runtime, first.Get("/a", {}, headers));
    auto b = mhttp::SyncWait(runtime, second.Get("/b", {}, headers));
    auto denied = mhttp::SyncWait(runtime, first.Get("/b", {}, headers));
    Check(a && b && a->Status() == mhttp::Status::Ok && b->Status() == mhttp::Status::Ok, "shared quota allows first two HTTP requests");
    Check(denied && denied->Status() == mhttp::Status::TooManyRequests && calls == 2, "excess request bypasses handler across routes and sockets");
    Check(denied && !denied->Header("Retry-After").empty() && denied->Header("Access-Control-Allow-Origin") == "*", "429 keeps retry hint and CORS finalization");
    auto other = mhttp::SyncWait(runtime, first.Get("/independent"));
    Check(other && other->Status() == mhttp::Status::Ok && calls == 3, "independent limiter retains its own quota");
    CheckEq(limiter.TrackedClients(), std::size_t{ 1 }, "different TCP source ports share ClientIp quota");
}

/**
 * @brief Use only resolved client identity, ignoring forged fields unless the transport peer is trusted.
 */
auto trusted_client_identity() -> void {
    for (bool trusted : { false, true }) {
        mhttp::Runtime runtime;
        Check(runtime.Start(4).has_value(), "start quota identity runtime");
        auto limiter = *mhttp::server::RateLimit::Create({ .max_requests = 1, .window = 60s });
        mhttp::server::ServerOptions options;
        if (trusted) { options.trusted_proxies = mhttp::server::TrustedProxies::Create({ "127.0.0.1" }).value(); }
        mhttp::server::HttpServer server{ runtime, options };
        Check(server.Use(limiter).has_value(), "install global quota");
        Check(server.Get("/", [](auto& request, auto& response) {
            static_cast<void>(response.SetContent(request.ClientIp(), "text/plain"));
        }).has_value(), "echo quota client identity");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_rate_server(server) };
        for (auto const& [address, expected] : std::to_array<std::pair<std::string_view, bool>>({
            { "192.0.2.1", true }, { "192.0.2.2", trusted }, { "192.0.2.1", false },
            { "2001:db8::1", trusted }, { "2001:0db8:0:0:0:0:0:1", false },
            { "invalid-address", trusted }, { "another-invalid-address", false } })) {
            mhttp::Headers headers;
            Check(headers.Set("X-Forwarded-For", address).has_value(), "forwarded quota identity");
            auto response = mhttp::SyncWait(runtime, client.Get("/", {}, std::move(headers)));
            Check(response && response->Status() == (expected ? mhttp::Status::Ok : mhttp::Status::TooManyRequests), "quota follows trusted normalized identity including malformed fallback");
            if (response && expected) {
                auto const wanted = !trusted || address == "invalid-address" ? std::string_view{ "127.0.0.1" } : address;
                CheckEq(response->Body(), wanted, "handler and limiter use the same ClientIp");
            }
        }
    }
}

/**
 * @brief Reject an exhausted lazy upload at its headers without invoking its provider or request handler.
 */
auto lazy_upload_denial() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start lazy quota runtime");
    auto limiter = *mhttp::server::RateLimit::Create({ .max_requests = 1, .window = 60s });
    mhttp::server::HttpServer server{ runtime };
    Check(server.Use(limiter).has_value(), "install lazy upload quota");
    Check(server.Get("/", [](auto&, auto& response) { static_cast<void>(response.SetContent("accepted", "text/plain")); }).has_value(), "register quota warmup");
    std::atomic<unsigned> handled{}, produced{};
    Check(server.RouteLazy(mhttp::Method::Post, "/upload", [&](auto&, auto&) { ++handled; }).has_value(), "register limited lazy upload");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_rate_server(server) };
    auto warmup = mhttp::SyncWait(runtime, client.Get());
    Check(warmup && warmup->Status() == mhttp::Status::Ok, "consume upload client's quota");
    mhttp::client::Request request{ mhttp::Method::Post, "/upload" };
    Check(request.Headers().Set("Expect", "100-continue").has_value(), "negotiate upload before provider");
    Check(request.SetContentProvider(7, [&](std::uint64_t, std::size_t) -> mhttp::Task<mhttp::Result<std::string>> {
        ++produced;
        co_return std::string{ "payload" };
    }, "application/octet-stream").has_value(), "own lazy upload source");
    auto denied = mhttp::SyncWait(runtime, client.Send(std::move(request)));
    Check(denied && denied->Status() == mhttp::Status::TooManyRequests && !denied->KeepAlive(), "quota denies before sending 100 and closes unread-body session");
    Check(handled == 0 && produced == 0, "denied upload executes neither handler nor provider");
}

/**
 * @brief Admit exactly one shared quota across concurrent pooled TCP sessions.
 */
auto concurrent_http_quota() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start concurrent HTTP quota runtime");
    auto limiter = *mhttp::server::RateLimit::Create({ .max_requests = 7, .window = 60s });
    mhttp::server::HttpServer server{ runtime };
    Check(server.Use(limiter).has_value(), "install concurrent quota");
    std::atomic<unsigned> accepted{}, handled{};
    Check(server.Get("/", [&](auto&, auto& response) -> mhttp::Task<void> {
        ++handled;
        co_await mhttp::Sleep(2ms);
        static_cast<void>(response.SetContent("accepted", "text/plain"));
    }).has_value(), "register concurrent limited route");
    auto port = start_rate_server(server);
    mhttp::client::ClientPool pool{ runtime, { .max_per_origin = 12, .max_total = 12 } };
    std::vector<mhttp::test::ThreadTask<void>> callers;
    for (unsigned index{}; index < 12; ++index) {
        callers.emplace_back([&] {
            auto handle = mhttp::SyncWait(runtime, pool.Acquire("127.0.0.1", port));
            if (!Check(handle.has_value(), "obtain concurrent HTTP loan")) { return; }
            auto response = mhttp::SyncWait(runtime, (*handle)->Get());
            Check(response.has_value(), "concurrent quota returns an ordinary HTTP response");
            if (response && response->Status() == mhttp::Status::Ok) { ++accepted; }
            else { Check(response && response->Status() == mhttp::Status::TooManyRequests, "excess concurrent request gets 429"); }
        });
    }
    for (auto& caller : callers) { caller.Get(); }
    Check(accepted == 7 && handled == 7 && limiter.TrackedClients() == 1, "distinct sessions cannot exceed shared quota");
    CheckEq(pool.Stats().active, std::size_t{}, "all concurrent quota clients return their pool loans");
}

/**
 * @brief Run real HTTP quota scenarios.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "shared HTTP route quotas", shared_http_routes }, { "trusted client quota identity", trusted_client_identity },
        { "lazy upload quota denial", lazy_upload_denial }, { "concurrent HTTP quota", concurrent_http_quota },
    });
}
