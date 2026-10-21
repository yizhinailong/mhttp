/**
 * @file json_test.cpp
 * @brief Exercise optional JSON readers/writers as a consumer importing only the public mhttp.json module.
 */
import std;
import mhttp.json;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Build an independently owned request for JSON syntax, media-type, and nesting tests.
 * @param body Owned complete JSON bytes.
 * @param type Borrowed media declaration copied into request fields.
 * @return Request retaining all parser inputs independently of this call.
 */
auto json_request(std::string body, std::string_view type = "application/json") -> mhttp::server::Request {
    mhttp::detail::RequestHead head{ .method = "POST" };
    if (!type.empty()) { Check(head.headers.Set("Content-Type", type).has_value(), "set JSON media declaration"); }
    auto request = mhttp::detail::ServerRequestAccess::Create(std::move(head));
    mhttp::detail::ServerRequestAccess::SetBody(request, std::move(body));
    return request;
}

/**
 * @brief Parse fresh JSON values without aliasing bodies and reject invalid syntax, ambiguous types, and excessive depth.
 */
auto json_readers() -> void {
    for (auto type : { "application/json", "Application/JSON; charset=UTF-8", "APPLICATION/merge-patch+JSON" }) {
        auto request = json_request("{\"name\":\"你好\",\"array\":[true,null,2],\"nul\":\"a\\u0000b\"}", type);
        auto value = request.Json();
        Check(value && (*value)["name"] == "你好" && (*value)["nul"] == std::string{ "a\0b", 3 }, "JSON media types preserve Unicode and escaped binary values");
        if (!value) { continue; }
        (*value)["name"] = "modified";
        Check((*request.Json())["name"] == "你好", "each Json call owns a fresh independent tree");
    }
    for (auto body : { "", "{", "[1,]", "1 2", "/*comment*/{}", "{\"x\":}", "\"bad\xff\"" }) {
        auto parsed = json_request(body).Json();
        Check(!parsed && parsed.error() == mhttp::Error::InvalidRequest, "invalid JSON is recoverable without parser exceptions");
    }
    CheckEq(json_request(std::string{ "{}\0trailing", 11 }).Json().error(), mhttp::Error::InvalidRequest, "raw NUL cannot hide trailing bytes from the JSON lexer");
    for (auto type : { "", "text/plain", "application/x/y+json", "application/+json" }) {
        CheckEq(json_request("{}", type).Json().error(), mhttp::Error::InvalidRequest, "reject missing/non-JSON or malformed media type");
    }
    auto depth = json_request("[{\"x\":\"[{}]\\\"\"}]");
    Check(depth.Json({ .max_depth = 2 }).has_value(), "braces and escaped quotes inside a JSON string do not count as nesting");
    CheckEq(depth.Json({ .max_depth = 1 }).error(), mhttp::Error::BodyTooLarge, "nesting bound is enforced before materialization");
    CheckEq(depth.Json({ .max_depth = 0 }).error(), mhttp::Error::InvalidRequest, "zero nesting configuration is invalid");
    Check(json_request("true").Json() && *json_request("true").Json() == true, "scalar root is supported");
    mhttp::detail::RequestHead head;
    Check(head.headers.Add("Content-Type", "application/json").has_value(), "first JSON Content-Type");
    Check(head.headers.Add("Content-Type", "application/json").has_value(), "duplicate JSON Content-Type");
    auto duplicate = mhttp::detail::ServerRequestAccess::Create(std::move(head));
    mhttp::detail::ServerRequestAccess::SetBody(duplicate, "{}");
    CheckEq(duplicate.Json().error(), mhttp::Error::InvalidHeaders, "ambiguous JSON media declaration is rejected");
}

/**
 * @brief Serialize JSON synchronously and preserve existing message state if encoding fails.
 */
auto json_writers() -> void {
    mhttp::OrderedJson ordered;
    ordered["z"] = 1;
    ordered["a"] = 2;
    mhttp::server::Response response;
    Check(response.SetStatus(mhttp::Status::Created).has_value(), "set original response status");
    Check(response.SetJson(ordered).has_value(), "serialize ordered JSON object");
    CheckEq(response.Body(), "{\"z\":1,\"a\":2}"sv, "ordered JSON preserves member order");
    CheckEq(response.Status(), mhttp::Status::Created, "JSON serialization preserves status");
    CheckEq(response.Header("Content-Type"), "application/json"sv, "JSON writer selects media type");
    auto before = response.Body();
    mhttp::Json invalid = std::string{ "invalid\xff" };
    auto rejected = response.SetJson(invalid);
    Check(!rejected && rejected.error() == mhttp::Error::InvalidRequest, "invalid UTF-8 serialization becomes a recoverable error");
    Check(response.Body() == before && response.Header("Content-Type") == "application/json" && response.Status() == mhttp::Status::Created, "serialization failure leaves response unchanged");
    mhttp::client::Request request{ mhttp::Method::Post, "/json" };
    Check(request.SetJson(mhttp::Json::array({ 1, "two", nullptr })).has_value(), "request accepts array root");
    before = request.Body();
    Check(!request.SetJson(invalid) && request.Body() == before && request.ContentType() == "application/json", "request serialization is also atomic");
}

/**
 * @brief Round-trip empty objects, array roots, large JSON, and 15 deterministic random JSON values over real HTTP.
 */
auto json_http_round_trips() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start JSON integration runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Post("/json", [](auto& request, auto& response) {
        auto value = request.Json();
        if (!value) {
            Check(response.SetError(value.error() == mhttp::Error::BodyTooLarge
                ? mhttp::Status::ContentTooLarge : mhttp::Status::BadRequest).has_value(), "map JSON input error");
            return;
        }
        Check(response.SetJson(*value).has_value(), "echo independently parsed JSON");
    }).has_value(), "register JSON echo");
    auto port = server.BindToAnyPort("127.0.0.1");
    Check(port && server.Start() && server.WaitUntilReady(), "start loopback JSON peer");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port };
    std::vector<mhttp::Json> values{ mhttp::Json::object(), mhttp::Json::array({ 1, "array", false }),
        mhttp::Json{ { "large", std::string(33 * 1024, 'x') } } };
    std::mt19937 random{ 0x45012026 };
    for (unsigned round{}; round < 15; ++round) {
        mhttp::Json value;
        value["round"] = round;
        value["number"] = static_cast<std::int64_t>(random()) - 0x80000000LL;
        value["fraction"] = static_cast<double>(random()) / 1024;
        value["flag"] = (random() % 2) == 0;
        value["nested"] = mhttp::Json::array({ nullptr, "你好 + & % \\\"", std::string{ "a\0b", 3 }, random() });
        values.emplace_back(std::move(value));
    }
    for (auto const& expected : values) {
        mhttp::client::Request request{ mhttp::Method::Post, "/json" };
        Check(request.SetJson(expected).has_value(), "materialize JSON before issuing lazy task");
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        Check(result && result->Status() == mhttp::Status::Ok, "JSON request/response succeeds");
        if (result) {
            auto actual = result->Json();
            Check(actual && *actual == expected, "JSON HTTP round trip preserves logical value");
        }
    }
    auto bad = mhttp::SyncWait(runtime, client.Post("/json", "{", "application/json"));
    Check(bad && bad->Status() == mhttp::Status::BadRequest, "invalid JSON reaches handler as a recoverable 400");
    mhttp::client::HttpClient bounded{ runtime, "127.0.0.1", *port, { .body_bytes = 32 } };
    mhttp::client::Request large{ mhttp::Method::Post, "/json" };
    Check(large.SetJson(mhttp::Json{ { "data", std::string(100, 'x') } }).has_value(), "set bounded-response input");
    auto limited = mhttp::SyncWait(runtime, bounded.Send(std::move(large)));
    Check(!limited && limited.error() == mhttp::Error::BodyTooLarge, "optional JSON preserves existing HTTP response body bound");
}

/**
 * @brief Exercise every JSON shortcut, synchronous ownership, field precedence, and failure without network dispatch.
 */
auto json_client_shortcuts() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start JSON shortcut runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> calls{};
    for (auto method : { mhttp::Method::Get, mhttp::Method::Head, mhttp::Method::Post, mhttp::Method::Put,
                         mhttp::Method::Patch, mhttp::Method::Delete, mhttp::Method::Options }) {
        Check(server.Route(method, "/shortcut", [&calls](auto& request, auto& response) {
            ++calls;
            Check(response.Headers().Set("X-Method", request.MethodString()).has_value(), "echo JSON method");
            Check(response.Headers().Set("X-Type", request.ContentType()).has_value(), "echo JSON request media type");
            Check(response.Headers().Set("X-Target", request.Target()).has_value(), "echo JSON request target");
            Check(response.Headers().Set("X-Body-Bytes", std::to_string(request.Body().size())).has_value(), "echo JSON body size even for HEAD");
            Check(response.Headers().Set("X-Explicit", request.Header("X-Explicit")).has_value(), "echo explicit JSON fields");
            Check(response.SetContent(request.Body(), "application/json").has_value(), "echo serialized JSON bytes");
        }).has_value(), "register JSON shortcut method");
    }
    auto port = server.BindToAnyPort("127.0.0.1");
    Check(port && server.Start() && server.WaitUntilReady(), "start JSON shortcut loopback peer");
    mhttp::client::ClientOptions options;
    Check(options.default_headers.Set("Content-Type", "text/plain").has_value(), "JSON media type overrides client defaults");
    Check(options.default_headers.Set("X-Explicit", "default").has_value(), "set overridden fallback field");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port, std::move(options) };
    mhttp::QueryParams query;
    query.Add("tag", "one two");
    query.Add("tag", "a/b");
    mhttp::Headers headers;
    Check(headers.Set("X-Explicit", "supplied").has_value(), "set explicit JSON field");
    mhttp::Json value{ { "message", "owned 你好" }, { "array", mhttp::Json::array({ 1, false, nullptr }) } };
    auto expected = value.dump();
    auto path = "/shortcut?base=1#local";
    auto tasks = std::array{
        client.Get(path, value, query, headers), client.Head(path, value, query, headers),
        client.Post(path, value, query, headers), client.Put(path, value, query, headers),
        client.Patch(path, value, query, headers), client.Delete(path, value, query, headers),
        client.Options(path, value, query, headers),
    };
    value.clear();
    Check(!client.IsOpen() && calls == 0, "serializing JSON shortcuts does not start network I/O");
    std::optional<mhttp::Endpoint> connection;
    std::size_t index{};
    for (auto& task : tasks) {
        auto result = mhttp::SyncWait(runtime, std::move(task));
        if (!Check(result.has_value(), "JSON shortcut completes")) { ++index; continue; }
        constexpr auto METHODS = std::array{ "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
        CheckEq(result->Header("X-Method"), METHODS[index], "selected JSON overload sends the requested method");
        CheckEq(result->Body(), index == 1 ? std::string{} : expected, "JSON input is owned before Task suspension; HEAD omits response bytes");
        CheckEq(result->Header("X-Body-Bytes"), std::to_string(expected.size()), "HEAD and ordinary methods carry the complete JSON request body");
        CheckEq(result->Header("X-Type"), "application/json"sv, "JSON fallback overrides default media type");
        CheckEq(result->Header("X-Target"), "/shortcut?base=1&tag=one+two&tag=a%2Fb"sv, "JSON overload retains ordered query encoding and drops fragment");
        CheckEq(result->Header("X-Explicit"), "supplied"sv, "JSON overload retains explicit header precedence");
        if (connection) { CheckEq(result->LocalEndpoint(), *connection, "JSON shortcuts reuse the framed connection"); }
        else { connection = result->LocalEndpoint(); }
        ++index;
    }
    CheckEq(calls.load(), 7U, "all seven JSON methods dispatch exactly once");
    for (auto type : { "application/merge-patch+json", "" }) {
        mhttp::Headers explicit_type;
        Check(explicit_type.Set("Content-Type", type).has_value(), "supply explicit JSON Content-Type override");
        auto result = mhttp::SyncWait(runtime, client.Patch("/shortcut", mhttp::Json::object(), {}, explicit_type));
        Check(result && result->Header("X-Type") == type && result->Body() == "{}", "explicit JSON media type wins even when empty");
    }
    mhttp::OrderedJson ordered;
    ordered["z"] = 1;
    ordered["a"] = 2;
    auto ordered_task = client.Post("/shortcut", ordered);
    ordered.clear();
    auto ordered_result = mhttp::SyncWait(runtime, std::move(ordered_task));
    Check(ordered_result && ordered_result->Body() == "{\"z\":1,\"a\":2}", "JSON shortcuts support insertion-ordered values with independent lifetime");
    auto count = calls.load();
    mhttp::Json invalid = std::string{ "invalid\xff" };
    auto rejected = mhttp::SyncWait(runtime, client.Post("/shortcut", invalid));
    Check(!rejected && rejected.error() == mhttp::Error::InvalidRequest && calls == count, "serialization failure returns a recoverable result without dispatch");
    Check(client.IsOpen(), "failed JSON construction preserves the existing connection");
    auto scalar = mhttp::SyncWait(runtime, client.Post("/shortcut", mhttp::Json(true)));
    Check(scalar && scalar->Body() == "true" && scalar->LocalEndpoint() == *connection, "temporary scalar JSON and post-failure connection reuse work");
    auto raw = mhttp::SyncWait(runtime, client.Post("/shortcut", "raw body", "text/plain"));
    Check(raw && raw->Body() == "raw body" && raw->Header("X-Type") == "text/plain", "string overload keeps raw body semantics");
    auto query_only = mhttp::SyncWait(runtime, client.Get("/shortcut", query));
    Check(query_only && query_only->Body().empty() && query_only->Header("X-Target") == "/shortcut?tag=one+two&tag=a%2Fb", "QueryParams overload remains distinct from JSON");
}

/**
 * @brief Rewrite errors from different layers into one JSON envelope while retaining status and required fields.
 */
auto json_error_responses() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start JSON error runtime");
    mhttp::server::HttpServer server{ runtime };
    std::atomic<unsigned> errors{}, posts{};
    Check(server.SetErrorHandler([&errors](auto&, auto& response) -> mhttp::Task<void> {
        ++errors;
        co_await mhttp::Sleep(std::chrono::milliseconds{ 1 });
        Check(response.SetJson(mhttp::Json{ { "error", {
            { "code", static_cast<unsigned>(response.Status()) },
            { "message", mhttp::ReasonPhrase(response.Status()) },
        } } }).has_value(), "serialize one application-selected JSON error format");
    }).has_value(), "register asynchronous JSON error transformer");
    Check(server.SetPostRoutingHandler([&posts](auto&, auto& response) {
        ++posts;
        Check(response.Headers().Set("X-Final", "once").has_value(), "finalize customized JSON error");
    }).has_value(), "register JSON error finalizer");
    Check(server.Post("/input", [](auto& request, auto& response) {
        if (!request.Json()) { Check(response.SetError(mhttp::Status::BadRequest).has_value(), "reject malformed JSON request"); }
    }).has_value(), "register JSON validation route");
    auto auth = mhttp::server::BearerAuth::Create("valid-token", "REST");
    Check(auth.has_value(), "create JSON error authentication policy");
    Check(server.Get("/protected", [](auto&, auto&) { Check(false, "missing Bearer token cannot dispatch"); }, *auth).has_value(), "register JSON authentication rejection");
    Check(server.Get("/throw", [](auto&, auto&) { throw std::runtime_error{ "private secret" }; }).has_value(), "register JSON exception boundary");
    auto port = server.BindToAnyPort("127.0.0.1");
    if (!port || !server.Start() || !server.WaitUntilReady()) { throw std::runtime_error{ "cannot start JSON error peer" }; }
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port };
    struct Case { mhttp::Method method; std::string_view path; mhttp::Status status; };
    for (auto const& test : {
             Case{ mhttp::Method::Get, "/missing", mhttp::Status::NotFound },
             Case{ mhttp::Method::Get, "/input", mhttp::Status::MethodNotAllowed },
             Case{ mhttp::Method::Post, "/input", mhttp::Status::BadRequest },
             Case{ mhttp::Method::Get, "/protected", mhttp::Status::Unauthorized },
             Case{ mhttp::Method::Get, "/throw", mhttp::Status::InternalServerError } }) {
        mhttp::client::Request request{ test.method, std::string{ test.path } };
        if (test.method == mhttp::Method::Post) { Check(request.SetContent("{", "application/json").has_value(), "send invalid JSON input"); }
        auto result = mhttp::SyncWait(runtime, client.Send(std::move(request)));
        if (!Check(result.has_value(), "customized REST error remains an ordinary HTTP response")) { continue; }
        CheckEq(result->Status(), test.status, "JSON formatter preserves error status");
        auto value = result->Json();
        Check(value && (*value)["error"]["code"] == static_cast<unsigned>(test.status)
            && (*value)["error"]["message"] == std::string{ mhttp::ReasonPhrase(test.status) }, "all REST error sources share the selected JSON envelope");
        CheckEq(result->Header("X-Final"), "once"sv, "post hook runs after JSON error transformation");
        CheckEq(result->Header("Content-Length"), std::to_string(result->Body().size()), "JSON error framing uses transformed bytes");
        Check(!result->Body().contains("secret"), "exception diagnostics stay outside the public error representation");
        if (test.status == mhttp::Status::MethodNotAllowed) { CheckEq(result->Header("Allow"), "POST"sv, "JSON error preserves Allow"); }
        if (test.status == mhttp::Status::Unauthorized) { CheckEq(result->Header("WWW-Authenticate"), "Bearer realm=\"REST\""sv, "JSON error preserves authentication challenge"); }
        if (test.status == mhttp::Status::InternalServerError) { Check(!result->KeepAlive(), "JSON exception response closes despite formatting"); }
    }
    Check(errors == 5 && posts == 5, "one error and post callback per transformed response");
}

/**
 * @brief Run optional JSON consumer acceptance independently of the feature-free test suite.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Bounded test process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "optional JSON readers", json_readers },
        { "optional JSON writers", json_writers },
        { "optional JSON HTTP round trips", json_http_round_trips },
        { "optional JSON client shortcuts", json_client_shortcuts },
        { "optional JSON error responses", json_error_responses },
    });
}
