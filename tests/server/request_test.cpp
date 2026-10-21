/**
 * @file request_test.cpp
 * @brief Verify owned request accessors, path decoding boundaries, strict capture conversion, and context.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;
using mhttp::detail::ServerRequestAccess;

/**
 * @brief Own metadata independently and distinguish encoded target, decoded path, and decoded queries.
 */
auto request_metadata() -> void {
    mhttp::detail::RequestHead head{ .method = "POST", .target = "/files/a%2Fb?tag=one+two&tag=three&flag" };
    Check(head.headers.Add("Host", "example.invalid").has_value(), "host field");
    Check(head.headers.Add("X-Tag", "one").has_value(), "first repeated field");
    Check(head.headers.Add("x-tag", "two").has_value(), "second repeated field");
    Check(head.headers.Add("Content-Type", "text/plain; charset=utf-8").has_value(), "content type field");
    auto request = ServerRequestAccess::Create(std::move(head), { "127.0.0.1", 1234 }, { "::1", 8080 });
    head.target = "/changed";
    CheckEq(request.Method(), std::optional{ mhttp::Method::Post }, "recognized method");
    CheckEq(request.MethodString(), "POST"sv, "raw method token");
    CheckEq(request.Target(), "/files/a%2Fb?tag=one+two&tag=three&flag"sv, "original encoded target is owned");
    CheckEq(request.Path(), "/files/a/b"sv, "application sees decoded path");
    CheckEq(ServerRequestAccess::EncodedPath(request), "/files/a%2Fb"sv, "routing keeps encoded separator boundary");
    CheckEq(request.Query().Values("tag"), std::vector{ "one two"sv, "three"sv }, "query decodes plus and preserves repetitions");
    Check(request.Query().Has("flag") && request.Query().Get("flag").empty(), "query flag");
    CheckEq(request.Headers().Values("X-Tag"), std::vector{ "one"sv, "two"sv }, "ordered immutable fields");
    CheckEq(request.Header("HOST"), "example.invalid"sv, "header case folding");
    CheckEq(request.Header("missing", "fallback"), "fallback"sv, "header fallback");
    CheckEq(request.ContentType(), "text/plain; charset=utf-8"sv, "content type retains parameters");
    CheckEq(request.RemoteEndpoint(), mhttp::Endpoint{ "127.0.0.1", 1234 }, "peer endpoint value");
    CheckEq(request.ClientIp(), "127.0.0.1", "client IP defaults to real peer");
    CheckEq(request.LocalEndpoint(), mhttp::Endpoint{ "::1", 8080 }, "local endpoint value");
    CheckEq(request.Version(), mhttp::Version::Http11, "wire version");
    Check(request.KeepAlive() && request.Body().empty(), "body initially empty and HTTP/1.1 persistent");
    ServerRequestAccess::SetBody(request, std::string{ "a\0b"sv });
    CheckEq(request.Body(), "a\0b"sv, "body owns binary bytes");
    mhttp::Headers trailers;
    Check(trailers.Add("X-Sum", "42").has_value(), "trailer field");
    ServerRequestAccess::SetTrailers(request, std::move(trailers));
    CheckEq(request.Trailers().Get("x-sum"), "42"sv, "trailers accessible separately");
    Check(!request.Headers().Has("X-Sum"), "trailer is not merged into request headers");
    request.Data().Store("user", std::string{ "alice" });
    auto moved = std::move(request);
    CheckEq(std::as_const(moved).Data().Fetch<std::string>("user"), std::optional<std::string>{ "alice" }, "context survives ownership transfer");
    CheckEq(moved.Body(), "a\0b"sv, "metadata and content survive move");
    static_assert(!std::is_copy_constructible_v<mhttp::server::Request>);
    static_assert(std::is_nothrow_move_constructible_v<mhttp::server::Request>);
}

/**
 * @brief Populate route captures from encoded input and verify strict numeric/string conversions.
 */
auto request_path_params() -> void {
    auto request = ServerRequestAccess::Create({ .target = "/files/a%2Fb?x=1" });
    mhttp::detail::RouteTree routes;
    Check(routes.Insert(mhttp::Method::Get, "/files/:name", 1).has_value(), "single segment route");
    auto match = routes.Match(request.MethodString(), ServerRequestAccess::EncodedPath(request));
    Check(match.handler == 1, "route sees one encoded file segment");
    ServerRequestAccess::SetPathParams(request, std::move(match.params));
    CheckEq(request.PathParam("name"), "a/b"sv, "decoded capture");
    CheckEq(request.PathParam<std::string>("name"), std::optional<std::string>{ "a/b" }, "owned string capture copy");
    CheckEq(request.PathParam<std::string_view>("name"), std::optional{ "a/b"sv }, "borrowed string capture");
    Check(!request.PathParam<std::string_view>("missing"), "missing string differs from an empty capture");
    CheckEq(request.PathParam("missing", "fallback"), "fallback"sv, "raw capture fallback");
    mhttp::QueryParams params;
    for (auto const& [name, value] : std::array<std::pair<std::string_view, std::string_view>, 10>{ {
             { "integer", "-42" }, { "float", "1.5" }, { "bool", "true" }, { "bad", "+1" }, { "tail", "1x" },
             { "large", "99999999999999999999999999" }, { "inf", "inf" }, { "empty", "" }, { "nul", "a\0b"sv }, { "*", "a//b/" }
         } }) { params.Add(name, value); }
    ServerRequestAccess::SetPathParams(request, std::move(params));
    CheckEq(request.PathParam<int>("integer"), std::optional{ -42 }, "strict signed integer");
    Check(!request.PathParam<unsigned>("integer"), "negative unsigned rejected");
    CheckEq(request.PathParam<double>("float"), std::optional{ 1.5 }, "floating value");
    CheckEq(request.PathParam<bool>("bool"), std::optional{ true }, "strict boolean");
    for (auto name : { "bad", "tail", "large", "empty", "missing" }) {
        Check(!request.PathParam<int>(name), std::format("invalid integer capture {}", name));
    }
    Check(!request.PathParam<double>("inf"), "nonfinite float rejected");
    CheckEq(request.PathParam<std::string_view>("empty"), std::optional{ ""sv }, "empty capture is present");
    CheckEq(request.PathParam("nul"), "a\0b"sv, "binary capture retained");
    CheckEq(request.PathParam("*"), "a//b/"sv, "wildcard capture available");
}

/**
 * @brief Preserve unknown methods and apply HTTP persistence defaults and complete Connection tokens.
 */
auto request_semantics() -> void {
    for (auto const& [version, connection, expected] : std::array<std::tuple<mhttp::Version, std::string_view, bool>, 6>{ {
             { mhttp::Version::Http11, "", true }, { mhttp::Version::Http11, "close", false },
             { mhttp::Version::Http11, "notclose", true }, { mhttp::Version::Http10, "", false },
             { mhttp::Version::Http10, "KEEP-ALIVE", true }, { mhttp::Version::Http10, "keep-alive, close", false }
         } }) {
        mhttp::detail::RequestHead head{ .method = "CUSTOM", .target = "/a+b/%00/%ZZ/%?n=42", .version = version };
        if (!connection.empty()) { Check(head.headers.Add("Connection", connection).has_value(), "connection field"); }
        auto request = ServerRequestAccess::Create(std::move(head));
        Check(!request.Method() && request.MethodString() == "CUSTOM", "unknown extension token preserved");
        CheckEq(request.KeepAlive(), expected, std::format("persistence for {:?}", connection));
        CheckEq(request.Path(), "/a+b/\0/%ZZ/%"sv, "path decoding keeps plus, NUL, and malformed escapes");
        CheckEq(request.Query().Get<int>("n"), std::optional{ 42 }, "typed query lookup");
    }
    auto asterisk = ServerRequestAccess::Create({ .method = "OPTIONS", .target = "*" });
    CheckEq(asterisk.Path(), "*"sv, "asterisk path");
    Check(asterisk.Query().Empty(), "asterisk has no query");
}

/**
 * @brief Run bounded server request scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "request metadata ownership", request_metadata },
        { "request path parameters", request_path_params },
        { "request method and persistence", request_semantics },
    });
}
