/**
 * @file message_test.cpp
 * @brief Verify client message ownership, atomic mutation rejection, binary bodies, and immutable response views.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Own outgoing request data and preserve the previous complete representation after invalid mutations.
 */
auto request_owned_mutations() -> void {
    std::string method{ "CUSTOM" }, target{ "/a%2Fb?existing=1#local" };
    mhttp::client::Request request{ method, target };
    method = "changed";
    target = "/changed";
    CheckEq(request.MethodString(), "CUSTOM"sv, "request owns extension method token");
    Check(!request.Method(), "extension token has no known method enumerator");
    CheckEq(request.Target(), "/a%2Fb?existing=1#local"sv, "request owns raw target including local fragment");
    request.Query().Add("tag", "one two");
    request.Query().Add("tag", "a/b");
    CheckEq(request.Query().Values("tag"), std::vector{ "one two"sv, "a/b"sv }, "additional query preserves caller order");
    Check(request.Headers().Add("X-Tag", "one").has_value(), "first outgoing field");
    Check(request.Headers().Add("x-tag", "two").has_value(), "second outgoing field");
    Check(request.SetContent(std::string{ "a\0b"sv }, "application/octet-stream").has_value(), "own binary request body");
    Check(request.SetContent(request.Body(), request.ContentType()).has_value(), "request accepts self-borrowed content and type");
    CheckEq(request.SetContent("changed", "text/plain\r\nInjected: yes").error(), mhttp::Error::InvalidHeaders,
        "invalid media type is rejected before replacing content");
    CheckEq(request.Body(), "a\0b"sv, "invalid mutation preserves binary body");
    CheckEq(request.ContentType(), "application/octet-stream"sv, "invalid mutation preserves content type");
    CheckEq(request.Headers().Values("X-Tag"), std::vector{ "one"sv, "two"sv }, "content mutation preserves unrelated fields");
    for (auto invalid : { ""sv, "bad method"sv, "GET\r\nInjected"sv, "a\0b"sv }) {
        CheckEq(request.SetMethod(invalid).error(), mhttp::Error::InvalidRequest, "reject invalid method token");
        CheckEq(request.MethodString(), "CUSTOM"sv, "invalid method mutation preserves extension token");
    }
    Check(request.SetMethod(request.MethodString()).has_value(), "method mutation accepts its own borrowed bytes");
    Check(request.SetMethod(mhttp::Method::Post).has_value(), "select known method");
    CheckEq(request.Method(), std::optional{ mhttp::Method::Post }, "known method accessor");
    CheckEq(request.SetMethod(static_cast<mhttp::Method>(999)).error(), mhttp::Error::InvalidRequest,
        "invalid method enum does not overwrite the selected method");
    Check(request.SetContent("untyped").has_value(), "request body does not require a media type");
    Check(!request.Headers().Has("Content-Type"), "absent media type removes the old field");
    request.SetTarget("/next");
    auto moved = std::move(request);
    CheckEq(moved.Target(), "/next"sv, "move retains target ownership");
    CheckEq(moved.Body(), "untyped"sv, "move retains representation ownership");
    CheckEq(std::as_const(moved).Query().Values("tag").size(), std::size_t{ 2 }, "move retains additional query collection");
    CheckEq(std::as_const(moved).Headers().Values("X-Tag").size(), std::size_t{ 2 }, "const request fields remain readable");
    static_assert(!std::is_copy_constructible_v<mhttp::client::Request>);
    static_assert(std::is_nothrow_move_constructible_v<mhttp::client::Request>);
}

/**
 * @brief Keep parsed response bytes, trailers, and endpoint snapshots independent of all factory inputs.
 */
auto response_owned_snapshot() -> void {
    mhttp::detail::ResponseHead head{ .version = mhttp::Version::Http10, .status = mhttp::Status::NotFound,
        .reason = "Custom missing" };
    Check(head.headers.Add("Set-Cookie", "one=1").has_value(), "first repeated response field");
    Check(head.headers.Add("set-cookie", "two=2").has_value(), "second repeated response field");
    Check(head.headers.Set("Content-Type", "application/octet-stream").has_value(), "parsed media type");
    mhttp::Headers trailers;
    Check(trailers.Set("X-Checksum", "42").has_value(), "parsed trailer");
    std::string body{ "x\0y"sv };
    mhttp::Endpoint remote{ "127.0.0.1", 8080 }, local{ "127.0.0.1", 12345 };
    auto response = mhttp::detail::ClientResponseAccess::Create(head, body, trailers, true, remote, local);
    head.reason.clear();
    head.headers.Clear();
    trailers.Clear();
    body = "changed";
    remote.address = "changed";
    local.port = 0;
    auto moved = std::move(response);
    CheckEq(moved.Status(), mhttp::Status::NotFound, "HTTP error is an ordinary response value");
    CheckEq(moved.Version(), mhttp::Version::Http10, "response retains received version");
    CheckEq(moved.Reason(), "Custom missing"sv, "response retains received reason bytes");
    CheckEq(moved.Headers().Values("SET-COOKIE"), std::vector{ "one=1"sv, "two=2"sv }, "response preserves repeated fields");
    CheckEq(moved.Header("missing", "fallback"), "fallback"sv, "response field fallback");
    CheckEq(moved.ContentType(), "application/octet-stream"sv, "response media type accessor");
    CheckEq(moved.Body(), "x\0y"sv, "response owns binary representation");
    CheckEq(moved.Trailers().Get("x-checksum"), "42"sv, "response owns separate trailers");
    Check(!moved.Headers().Has("X-Checksum"), "trailers are not merged into response fields");
    CheckEq(moved.RemoteEndpoint(), mhttp::Endpoint{ "127.0.0.1", 8080 }, "response owns remote endpoint");
    CheckEq(moved.LocalEndpoint(), mhttp::Endpoint{ "127.0.0.1", 12345 }, "response owns local endpoint");
    Check(moved.KeepAlive(), "response retains finalized reuse policy");
    static_assert(!std::is_copy_constructible_v<mhttp::client::Response>);
    static_assert(std::is_nothrow_move_constructible_v<mhttp::client::Response>);
    static_assert(std::same_as<decltype(moved.Headers()), mhttp::Headers const&>);
    static_assert(std::same_as<decltype(moved.Trailers()), mhttp::Headers const&>);
}

/**
 * @brief Keep default request/response states usable for asynchronous completion storage without inferring content.
 */
auto message_defaults() -> void {
    mhttp::client::Request request;
    CheckEq(request.Method(), std::optional{ mhttp::Method::Get }, "default request is GET");
    CheckEq(request.Target(), "/"sv, "default request target is root");
    Check(request.Body().empty() && request.Headers().Empty() && request.Query().Empty(), "default request has no inferred fields or content");
    mhttp::client::Response response;
    CheckEq(response.Status(), mhttp::Status::Unknown, "default response does not pretend a peer returned 200");
    Check(response.Body().empty() && response.Headers().Empty() && response.Trailers().Empty(), "default response owns empty data");
    Check(response.Reason().empty() && !response.KeepAlive(), "unparsed response has no peer metadata or reuse permission");
    CheckEq(response.RemoteEndpoint(), mhttp::Endpoint{}, "default peer endpoint is empty");
}

/**
 * @brief Run client message ownership scenarios with bounded per-case watchdogs.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Process test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client request owned mutations", request_owned_mutations },
        { "client response owned snapshot", response_owned_snapshot },
        { "client message defaults", message_defaults },
    });
}
