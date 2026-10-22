/**
 * @file redirect_test.cpp
 * @brief Verify bounded redirects, method/body rewriting, effective URLs, and cross-origin configuration.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

/**
 * @brief Start a configured loopback listener on a dynamic port.
 * @param server Longer-lived configured listener.
 * @return Selected TCP port.
 */
auto start_redirect_peer(mhttp::server::HttpServer& server) -> std::uint16_t {
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "redirect peer failed" }; }
    return *port;
}

/**
 * @brief Preserve 307/308 bodies and HEAD on 301/302, and remove representation fields when rewriting to GET.
 */
auto methods_and_bodies() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start redirect method runtime");
    mhttp::server::HttpServer server{ runtime };
    for (auto status : { mhttp::Status::MovedPermanently, mhttp::Status::Found, mhttp::Status::SeeOther,
                         mhttp::Status::TemporaryRedirect, mhttp::Status::PermanentRedirect }) {
        Check(server.set_http_handler<mhttp::method::Post, mhttp::method::Head>(
            "/redirect/" + std::to_string(static_cast<int>(status)), [status](auto&, auto& response) {
            Check(response.SetRedirect("/final", status).has_value(), "set method redirect");
            Check(response.SetContent("discarded redirect representation", "").has_value(), "redirect has its own body");
        }).has_value(), "register method redirect");
    }
    Check(server.set_http_handler<mhttp::method::Get, mhttp::method::Post, mhttp::method::Head>(
        "/final", [](auto& request, auto& response) {
        Check(response.Headers().Set("X-Method", request.MethodString()).has_value(), "echo redirected method");
        Check(response.Headers().Set("X-Bytes", std::to_string(request.Body().size())).has_value(), "echo redirected length");
        Check(response.Headers().Set("X-Type", request.ContentType()).has_value(), "echo redirected media type");
        Check(response.SetContent(request.Body(), "").has_value(), "echo retained representation");
    }).has_value(), "register final echo");
    Check(server.set_http_handler<mhttp::method::Get>("/probe", [](auto&, auto& response) {
        Check(response.SetContent("clean stream", "").has_value(), "probe after redirected body");
    }).has_value(), "register stream probe");
    auto port = start_redirect_peer(server);
    mhttp::client::ClientOptions options{ .max_redirects = 1 };
    Check(options.default_headers.Set("Content-Type", "fallback/type").has_value(), "default representation field");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", port, options };
    for (auto code : { 301, 302, 303, 307, 308 }) {
        for (auto method : { mhttp::Method::Post, mhttp::Method::Head }) {
            bool const rewritten = code == 303 || ((code == 301 || code == 302) && method != mhttp::Method::Head);
            mhttp::client::Request request{ method, "/redirect/" + std::to_string(code) };
            Check(request.SetContent(std::string{ "a\0b", 3 }, "application/octet-stream").has_value(), "set binary redirect body");
            auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
            Check(result && result->Status() == mhttp::Status::Ok, "redirect reaches final handler");
            if (!result) { continue; }
            CheckEq(result->Header("X-Method"), rewritten ? "GET"sv : mhttp::ToString(method), "status determines next method");
            CheckEq(result->Header("X-Bytes"), rewritten ? "0"sv : "3"sv, "body is dropped only when method is rewritten");
            CheckEq(result->Header("X-Type"), rewritten ? ""sv : "application/octet-stream"sv, "representation fields match retained body");
            if (method != mhttp::Method::Head) {
                CheckEq(result->Body(), rewritten ? ""sv : "a\0b"sv, "caller sees only the final response representation");
            }
            auto probe = mhttp::SyncWait(runtime, client.Get("/probe"));
            Check(probe && probe->Body() == "clean stream" && probe->LocalEndpoint() == result->LocalEndpoint(), "redirect leaves no stray request/response body bytes");
        }
    }
}

/**
 * @brief Return ordinary 3xx by default and bound a followed loop without damaging future requests.
 */
auto disabled_and_bounded() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start redirect limit runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> calls{};
    Check(server.set_http_handler<mhttp::method::Get>("/loop", [&calls](auto&, auto& response) {
        ++calls;
        Check(response.SetRedirect("/loop").has_value(), "redirect loop");
        Check(response.SetContent("original redirect", "").has_value(), "set original 302 body");
    }).has_value(), "register loop");
    Check(server.set_http_handler<mhttp::method::Get>("/missing", [](auto&, auto& response) {
        Check(response.SetStatus(mhttp::Status::Found).has_value(), "set response status");
        Check(response.SetContent("no Location", "").has_value(), "302 without Location");
    }).has_value(), "register missing Location");
    Check(server.set_http_handler<mhttp::method::Get>("/cached", [](auto&, auto& response) {
        Check(response.SetStatus(mhttp::Status::NotModified).has_value(), "set response status");
        Check(response.Headers().Set("Location", "/loop").has_value(), "304 is not a redirect");
    }).has_value(), "register 304");
    Check(server.set_http_handler<mhttp::method::Get>("/duplicate", [](auto&, auto& response) {
        Check(response.SetStatus(mhttp::Status::Found).has_value(), "set response status");
        Check(response.Headers().Add("Location", "/loop").has_value(), "first Location");
        Check(response.Headers().Add("Location", "/missing").has_value(), "ambiguous Location");
    }).has_value(), "register duplicate Location");
    Check(server.set_http_handler<mhttp::method::Get>("/tls", [](auto&, auto& response) {
        Check(response.SetRedirect("https://localhost/").has_value(), "unsupported scheme Location");
    }).has_value(), "register unsupported redirect scheme");
    auto port = start_redirect_peer(server);
    mhttp::client::HttpClient ordinary{ runtime, "127.0.0.1", port };
    auto response = mhttp::SyncWait(runtime, ordinary.Get("/loop"));
    Check(response && response->Status() == mhttp::Status::Found && response->Body() == "original redirect", "following is disabled by default");
    CheckEq(calls.load(), 1U, "default sends exactly one request");
    mhttp::client::HttpClient following{ runtime, "127.0.0.1", port, { .max_redirects = 2 } };
    auto loop = mhttp::SyncWait(runtime, following.Get("/loop"));
    Check(!loop && loop.error() == mhttp::Error::ExceedRedirectCount, "loop exceeding configured count is an error");
    CheckEq(calls.load(), 4U, "only two additional hops are allowed");
    Check(!following.IsOpen(), "redirect failure closes transport");
    auto missing = mhttp::SyncWait(runtime, following.Get("/missing"));
    Check(missing && missing->Status() == mhttp::Status::Found && missing->Body() == "no Location", "missing Location returns original 3xx");
    auto cached = mhttp::SyncWait(runtime, following.Get("/cached"));
    Check(cached && cached->Status() == mhttp::Status::NotModified, "304 never follows Location");
    auto duplicate = mhttp::SyncWait(runtime, following.Get("/duplicate"));
    Check(!duplicate && duplicate.error() == mhttp::Error::InvalidHeaders, "reject ambiguous redirect destination");
    auto tls = mhttp::SyncWait(runtime, following.Get("/tls"));
    Check(!tls && tls.error() == mhttp::Error::UnsupportedScheme, "redirect cannot silently enable TLS");
}

/**
 * @brief Resolve Location against the actual sent target while preserving escapes, pluses, and query order.
 */
auto relative_targets() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start relative redirect runtime");
    mhttp::server::HttpServer server{ runtime };
    std::string location;
    Check(server.set_http_handler<mhttp::method::Get>("/dir/start", [&location](auto&, auto& response) {
        Check(response.SetRedirect(location).has_value(), "configure relative Location");
    }).has_value(), "register relative source");
    for (auto path : { "/next", "/dir/next", "/a+b/:segment" }) {
        Check(server.set_http_handler<mhttp::method::Get>(path, [](auto& request, auto& response) {
            Check(response.SetContent(std::string{ request.Target() }, "").has_value(), "echo unmodified final wire target");
            Check(response.Headers().Set("X-Cookie", request.Header("Cookie")).has_value(), "echo same-origin cookie");
        }).has_value(), "register effective relative destination");
    }
    auto port = start_redirect_peer(server);
    mhttp::client::HttpClient client{ runtime, "http://127.0.0.1:" + std::to_string(port) + "/wrong/base", { .max_redirects = 2 } };
    for (auto const& [reference, expected] : std::to_array<std::pair<std::string_view, std::string_view>>({
             { "../next?x=a+b&x=%2B#fragment", "/next?x=a+b&x=%2B" },
             { "next?q=1", "/dir/next?q=1" }, { "/a+b/c%2Fd?q=+%2B", "/a+b/c%2Fd?q=+%2B" } })) {
        location = reference;
        mhttp::client::Request request{ mhttp::Method::Get, "/dir/start?old=1#local" };
        request.Query().Add("extra", "once");
        Check(request.Headers().Set("Cookie", "session=private").has_value(), "same-origin cookie");
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(result.has_value(), std::format("relative redirect {} completes", reference));
        if (result) {
            CheckEq(result->Body(), expected, std::format("Location {} sends target {}, received {} with status {}",
                reference, expected, result->Body(), static_cast<int>(result->Status())));
            CheckEq(result->Header("X-Cookie"), "session=private"sv, "same-origin redirect retains cookie");
        }
    }
    location = "http://127.0.0.1:" + std::to_string(port) + "/next?absolute=yes#ignored";
    auto absolute = mhttp::SyncWait(runtime, client.Get("/dir/start"));
    Check(absolute && absolute->Body() == "/next?absolute=yes", "absolute same-origin Location follows without fragment");
}

/**
 * @brief Remove explicit and configured credentials across ports, even after a redirect returns to the first origin.
 */
auto cross_origin_credentials() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start cross-origin runtime");
    mhttp::server::HttpServer first{ runtime }, second{ runtime };
    auto first_port = first.BindToAnyPort("127.0.0.1");
    auto second_port = second.BindToAnyPort("127.0.0.1");
    Check(first_port && second_port, "bind both redirect origins");
    Check(first.set_http_handler<mhttp::method::Get>("/cross", [second_port](auto& request, auto& response) {
        Check(!request.Header("Authorization").empty() && !request.Header("Cookie").empty(), "first hop receives credentials");
        Check(response.SetRedirect("http://127.0.0.1:" + std::to_string(*second_port) + "/remote").has_value(), "cross-port Location");
    }).has_value(), "register cross-origin source");
    Check(second.set_http_handler<mhttp::method::Get>("/remote", [first_port, second_port](auto& request, auto& response) {
        Check(request.Header("Authorization").empty() && request.Header("Cookie").empty()
            && request.Header("Proxy-Authorization").empty(), "neither explicit nor fallback credentials cross origin");
        CheckEq(request.Header("Host"), "127.0.0.1:" + std::to_string(*second_port), "cross-origin Host is regenerated");
        CheckEq(request.Header("X-Default"), "retained"sv, "ordinary defaults are retained on every hop");
        Check(response.SetRedirect("http://127.0.0.1:" + std::to_string(*first_port) + "/final").has_value(), "redirect back to first origin");
    }).has_value(), "register remote origin");
    Check(first.set_http_handler<mhttp::method::Get>("/final", [](auto& request, auto& response) {
        Check(request.Header("Authorization").empty() && request.Header("Cookie").empty(), "returning to original origin does not restore removed credentials");
        Check(response.SetContent("final", "").has_value(), "finish cross-origin chain");
    }).has_value(), "register returning destination");
    Check(first.set_http_handler<mhttp::method::Get>("/direct", [](auto& request, auto& response) {
        Check(!request.Header("Authorization").empty() && !request.Header("Cookie").empty(), "new Send retains configured credentials");
        Check(response.SetContent("direct", "").has_value(), "direct request after redirects");
    }).has_value(), "register direct destination");
    Check(first.Start() && second.Start() && first.WaitUntilReady() && second.WaitUntilReady(), "start two origins");
    for (unsigned kind{}; kind < 3; ++kind) {
        mhttp::client::ClientOptions options{ .max_redirects = 2 };
        Check(options.default_headers.Set("X-Default", "retained").has_value(), "ordinary fallback field");
        Check(options.default_headers.Set("Cookie", "default=private").has_value(), "fallback cookie");
        Check(options.default_headers.Set("Host", "virtual.local").has_value(), "explicit virtual authority for original origin");
        mhttp::client::HttpClient client{ runtime, "127.0.0.1", *first_port, options };
        if (kind == 0) { Check(client.SetBasicAuth("user", "pass").has_value(), "configured Basic auth"); }
        else { Check(client.SetBearerToken("secret").has_value(), "configured Bearer auth"); }
        mhttp::client::Request request{ mhttp::Method::Get, "/cross" };
        if (kind == 2) {
            Check(request.Headers().Set("Authorization", "Custom explicit").has_value(), "explicit authorization");
            Check(request.Headers().Set("Cookie", "explicit=private").has_value(), "explicit cookie");
            Check(request.Headers().Set("Proxy-Authorization", "Basic cHJveHk=").has_value(), "explicit proxy credential");
        }
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(result && result->Body() == "final", "cross-origin chain completes");
        auto direct = mhttp::SyncWait(runtime, client.Get("/direct"));
        Check(direct && direct->Body() == "direct", "local stripping leaves client configuration intact");
    }
}

/**
 * @brief Apply response bounds and deadlines on a new redirect transport, and return later Sends to the constructor origin.
 */
auto cross_origin_limits() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start cross-origin limits runtime");
    mhttp::server::HttpServer first{ runtime }, second{ runtime };
    Check(second.set_http_handler<mhttp::method::Get>("/large", [](auto&, auto& response) {
        Check(response.SetContent("123456789", "").has_value(), "oversized destination body");
    }).has_value(), "register large destination");
    Check(second.set_http_handler<mhttp::method::Get>("/slow", [](auto&, auto& response) -> mhttp::Task<void> {
        co_await mhttp::Sleep(200ms);
        Check(response.SetContent("slow", "").has_value(), "delayed destination");
    }).has_value(), "register slow destination");
    Check(second.set_http_handler<mhttp::method::Get>("/ok", [](auto&, auto& response) {
        Check(response.SetContent("remote", "").has_value(), "complete remote hop");
    }).has_value(), "register remote success");
    auto second_port = start_redirect_peer(second);
    for (auto path : { "/large", "/slow", "/ok" }) {
        Check(first.set_http_handler<mhttp::method::Get>(path, [second_port, path](auto&, auto& response) {
            Check(response.SetRedirect("http://127.0.0.1:" + std::to_string(second_port) + path).has_value(), "cross-origin bounded redirect");
        }).has_value(), "register limit redirect");
    }
    Check(first.set_http_handler<mhttp::method::Get>("/direct", [](auto&, auto& response) {
        Check(response.SetContent("local", "").has_value(), "constructor origin destination");
    }).has_value(), "register original origin success");
    auto first_port = start_redirect_peer(first);
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", first_port,
        { .read_timeout = 50ms, .body_bytes = 8, .max_redirects = 1 } };
    auto large = mhttp::SyncWait(runtime, client.Get("/large"));
    Check(!large && large.error() == mhttp::Error::BodyTooLarge, "redirect preserves configured body bound");
    auto slow = mhttp::SyncWait(runtime, client.Get("/slow"));
    Check(!slow && slow.error() == mhttp::Error::ReadTimeout, "redirect preserves configured read timeout");
    auto remote = mhttp::SyncWait(runtime, client.Get("/ok"));
    Check(remote && remote->Body() == "remote", "finish on a different origin");
    auto local = mhttp::SyncWait(runtime, client.Get("/direct"));
    Check(local && local->Body() == "local" && local->RemoteEndpoint().port == first_port, "next Send reconnects to constructor origin");
    CheckEq(client.Port(), first_port, "redirect never mutates constructor destination accessors");
}

/**
 * @brief Run bounded redirect acceptance tests.
 * @param argc Argument count.
 * @param argv Optional test filters.
 * @return Test process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client redirect methods and bodies", methods_and_bodies },
        { "client redirect disabled and bounded", disabled_and_bounded },
        { "client redirect relative targets", relative_targets },
        { "client redirect cross-origin credentials", cross_origin_credentials },
        { "client redirect cross-origin limits", cross_origin_limits },
    });
}
