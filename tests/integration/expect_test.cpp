/**
 * @file expect_test.cpp
 * @brief Verify client/server upload negotiation, admission rejection, persistence, and redirect ownership.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;

/**
 * @brief Bind and start a public listener on an available loopback port.
 * @param server Longer-lived listener borrowing the active runtime.
 * @return Actual port or an exception if fixture setup fails.
 */
auto start_expect_server(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start Expect listener" }; }
    return *port;
}

/**
 * @brief Preserve binary request ownership through async handlers and reopen after a header-only route rejection.
 */
auto expect_binary_round_trip() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start Expect round-trip runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> uploads{};
    Check(server.set_http_handler<mhttp::method::Post>("/upload", [&](auto& request, auto& response) -> mhttp::Task<void> {
        ++uploads;
        co_await mhttp::Sleep(1ms);
        Check(response.Headers().Set("X-Expect", request.Header("Expect")).has_value(), "echo actual expectation field");
        Check(response.SetContent(request.Body(), "application/octet-stream").has_value(), "echo owned binary upload after suspension");
    }).has_value(), "register async Expect echo");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_expect_server(server),
        { .expect_continue_threshold = 1024 } };
    std::string body(1024 * 1024, '\0');
    for (std::size_t index{}; index < body.size(); ++index) { body[index] = static_cast<char>(index % 256); }
    std::optional<mhttp::Endpoint> persistent;
    for (unsigned repeat{}; repeat < 3; ++repeat) {
        auto result = mhttp::SyncWait(runtime, client.Post("/upload", body, "application/octet-stream"));
        Check(result && result->Status() == mhttp::Status::Ok && result->KeepAlive(), "large upload waits for server 100 and remains persistent");
        if (result) {
            CheckEq(result->Body(), body, "server/client preserve every binary byte");
            CheckEq(result->Header("X-Expect"), std::string_view{ "100-continue" }, "large upload inserts the negotiated field");
            if (persistent) { CheckEq(result->LocalEndpoint(), *persistent, "subsequent accepted uploads reuse one TCP connection"); }
            persistent = result->LocalEndpoint();
        }
    }
    auto rejected = mhttp::SyncWait(runtime, client.Post("/missing", body));
    Check(rejected && rejected->Status() == mhttp::Status::NotFound && !rejected->KeepAlive() && !client.IsOpen(),
        "unknown route rejects its head before upload and closes");
    auto reopened = mhttp::SyncWait(runtime, client.Post("/upload", body));
    Check(persistent && reopened && reopened->Body() == body && reopened->LocalEndpoint() != *persistent,
        "next accepted upload negotiates on a new connection");
    auto empty = mhttp::SyncWait(runtime, client.Post("/upload"));
    Check(empty && empty->Body().empty() && empty->Header("X-Expect").empty(), "empty request never acquires automatic Expect");
    Check(server.Stop().has_value(), "drain binary negotiation sessions");
    CheckEq(uploads.load(), 5U, "only accepted complete uploads reach the handler");
}

/**
 * @brief Reject oversized announced bodies before 100 and negotiate a later admissible upload normally.
 */
auto expect_server_admission() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start Expect admission runtime");
    mhttp::server::HttpServer server{ runtime, { .body_bytes = 1024 } };
    std::atomic<unsigned> calls{};
    Check(server.set_http_handler<mhttp::method::Post>("/upload", [&](auto& request, auto& response) {
        ++calls;
        Check(response.SetContent(request.Body(), "text/plain").has_value(), "echo bounded admitted body");
    }).has_value(), "register admission sentinel");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_expect_server(server),
        { .expect_continue_threshold = 128 } };
    auto rejected = mhttp::SyncWait(runtime, client.Post("/upload", std::string(2048, 'x')));
    Check(rejected && rejected->Status() == mhttp::Status::ContentTooLarge && !rejected->KeepAlive() && !client.IsOpen(),
        "oversized announced upload returns complete 413 before body admission");
    CheckEq(calls.load(), 0U, "rejected upload never reaches business code");
    auto accepted = mhttp::SyncWait(runtime, client.Post("/upload", std::string(512, 'y')));
    Check(accepted && accepted->Body() == std::string(512, 'y') && accepted->KeepAlive(), "new admissible upload receives 100 and completes");
    Check(server.Stop().has_value(), "drain admission sessions");
    CheckEq(calls.load(), 1U, "only the admitted upload dispatches");
}

/**
 * @brief Preserve pending bodies on early 307, drop them on 303, and retain replay storage after a 100 followed by 307.
 */
auto expect_redirect_ownership() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start Expect redirect runtime");
    mhttp::server::HttpServer destination{ runtime };
    std::string const body(128 * 1024, 'b');
    std::atomic<unsigned> posts{}, gets{}, early{}, after{};
    Check(destination.set_http_handler<mhttp::method::Post>("/post", [&](auto& request, auto& response) {
        ++posts;
        CheckEq(request.Body(), body, "redirect preserves an owned pending or already transmitted body");
        CheckEq(request.Header("Expect"), std::string_view{ "100-continue" }, "body-preserving redirect negotiates its own 100");
        Check(response.SetContent(request.Body(), "text/plain").has_value(), "return final redirected upload");
    }).has_value(), "register redirected upload");
    Check(destination.set_http_handler<mhttp::method::Get>("/get", [&](auto& request, auto& response) {
        ++gets;
        Check(request.Body().empty() && request.Header("Expect").empty(), "303 drops both promised body and Expect");
        Check(response.SetContent("rewritten", "text/plain").has_value(), "acknowledge redirected GET");
    }).has_value(), "register rewritten GET");
    auto const port = start_expect_server(destination);
    auto const origin = std::format("http://127.0.0.1:{}", port);
    mhttp::server::HttpServer source{ runtime };
    Check(source.SetPreRoutingHandler([&](auto& request, auto& response) -> bool {
        if (request.Path() == "/after") { return false; }
        ++early;
        Check(request.Body().empty(), "early redirect is decided before any request body is read");
        bool const rewrite = request.Path() == "/rewrite";
        Check(response.SetRedirect(origin + (rewrite ? "/get" : "/post"), rewrite
            ? mhttp::Status::SeeOther : mhttp::Status::TemporaryRedirect).has_value(), "set early cross-origin redirect");
        return true;
    }).has_value(), "register header-only redirects");
    Check(source.set_http_handler<mhttp::method::Post>("/after", [&](auto& request, auto& response) {
        ++after;
        CheckEq(request.Body(), body, "source fully receives the upload after its own 100");
        Check(response.SetRedirect(origin + "/post", mhttp::Status::TemporaryRedirect).has_value(), "redirect an already uploaded body");
    }).has_value(), "register post-upload redirect");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", start_expect_server(source),
        { .max_redirects = 2, .expect_continue_threshold = 1024 } };
    for (auto path : { "/keep", "/after" }) {
        auto result = mhttp::SyncWait(runtime, client.Post(path, body));
        Check(result && result->Body() == body && result->Status() == mhttp::Status::Ok,
            "307 reaches the destination with a complete owned upload");
    }
    mhttp::client::Request rewrite{ mhttp::Method::Post, "/rewrite" };
    Check(rewrite.SetContent(body).has_value(), "own a body for the early 303");
    Check(rewrite.Headers().Set("Expect", "100-continue").has_value(), "explicit expectation must also be stripped on rewrite");
    auto result = mhttp::SyncWait(runtime, client.Send(std::move(rewrite)));
    Check(result && result->Body() == "rewritten", "early 303 follows with an ordinary GET");
    Check(source.Stop() && destination.Stop(), "drain both redirect origins");
    CheckEq(early.load(), 2U, "both early redirects occur once");
    CheckEq(after.load(), 1U, "only the route that sends 100 consumes the source body");
    CheckEq(posts.load(), 2U, "each 307 negotiates and dispatches one destination upload");
    CheckEq(gets.load(), 1U, "303 dispatches one bodyless GET");
}

/**
 * @brief Run public upload negotiation integration with watchdog-bounded loopback scenarios.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test runner status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "Expect binary round trip", expect_binary_round_trip },
        { "Expect server admission", expect_server_admission },
        { "Expect redirect ownership", expect_redirect_ownership },
    });
}
