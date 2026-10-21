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
    });
}
