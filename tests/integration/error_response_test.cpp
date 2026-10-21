/**
 * @file error_response_test.cpp
 * @brief Verify one safe default error format across routing, validation, authentication, and exceptions over HTTP.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Keep application and router errors consistent while preserving required protocol fields and hiding diagnostics.
 */
auto default_rest_errors() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start REST error runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Get("/resource", [](auto&, auto&) {}).has_value(), "register method mismatch resource");
    Check(server.Get("/input", [](auto&, auto& response) {
        Check(response.SetError(mhttp::Status::BadRequest).has_value(), "reject invalid application input");
    }).has_value(), "register application validation error");
    auto auth = mhttp::server::BasicAuth::Create("user", "password", "REST");
    Check(auth.has_value(), "create REST authentication policy");
    Check(server.Get("/protected", [](auto&, auto&) {
        Check(false, "unauthorized request cannot reach the resource");
    }, *auth).has_value(), "register authentication rejection");
    Check(server.Get("/throw", [](auto&, auto& response) {
        Check(response.SetContent("private partial output", "application/octet-stream").has_value(), "prepare discarded response");
        throw std::runtime_error{ "private exception diagnostic" };
    }).has_value(), "register contained exception");
    std::atomic<unsigned> posts{};
    Check(server.SetPostRoutingHandler([&posts](auto&, auto& response) {
        ++posts;
        Check(response.Headers().Set("X-Final", "once").has_value(), "finalize every error response");
    }).has_value(), "register final response hook");
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start REST error peer" }; }
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port };
    struct Case { mhttp::Method method; std::string_view path; mhttp::Status status; };
    for (auto const& test : {
             Case{ mhttp::Method::Get, "/missing", mhttp::Status::NotFound },
             Case{ mhttp::Method::Post, "/resource", mhttp::Status::MethodNotAllowed },
             Case{ mhttp::Method::Get, "/input", mhttp::Status::BadRequest },
             Case{ mhttp::Method::Get, "/protected", mhttp::Status::Unauthorized },
             Case{ mhttp::Method::Get, "/throw", mhttp::Status::InternalServerError } }) {
        auto result = mhttp::SyncWait(runtime, client.Send(mhttp::client::Request{ test.method, std::string{ test.path } }));
        if (!Check(result.has_value(), "REST error remains a complete HTTP response")) { continue; }
        CheckEq(result->Status(), test.status, "preserve the originating error status");
        CheckEq(result->Body(), std::format("{} {}\n", static_cast<unsigned>(test.status), mhttp::ReasonPhrase(test.status)),
            "all default REST errors use code and static reason only");
        CheckEq(result->ContentType(), "text/plain; charset=utf-8"sv, "default error media type");
        CheckEq(result->Header("Content-Length"), std::to_string(result->Body().size()), "frame the final error bytes");
        CheckEq(result->Headers().Count("Content-Length"), std::size_t{ 1 }, "one authoritative error body length");
        CheckEq(result->Header("X-Final"), "once"sv, "post hook finalizes every error source");
        if (test.status == mhttp::Status::MethodNotAllowed) {
            CheckEq(result->Header("Allow"), "GET, HEAD"sv, "error formatting preserves required method information");
        }
        if (test.status == mhttp::Status::Unauthorized) {
            CheckEq(result->Header("WWW-Authenticate"), "Basic realm=\"REST\", charset=\"UTF-8\""sv, "error formatting preserves the challenge");
        }
        if (test.status == mhttp::Status::InternalServerError) { Check(!result->KeepAlive(), "exception errors still close the connection"); }
    }
    CheckEq(posts.load(), 5U, "one final hook per REST error response");
}

/**
 * @brief Run loopback REST error acceptance under a bounded watchdog.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, { { "default REST error format", default_rest_errors } });
}
