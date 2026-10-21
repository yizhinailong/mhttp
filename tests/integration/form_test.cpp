/**
 * @file form_test.cpp
 * @brief Verify on-demand URL-encoded form ownership and randomized form/query round trips over HTTP.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Keep form decoding separate from queries and original body storage while validating media types.
 */
auto form_media_and_ownership() -> void {
    mhttp::QueryParams retained;
    for (auto type : { "application/x-www-form-urlencoded", "Application/X-WWW-Form-Urlencoded; charset=UTF-8",
                       "application/x-www-form-urlencoded ; charset=unknown" }) {
        mhttp::detail::RequestHead head{ .method = "POST", .target = "/?field=query" };
        Check(head.headers.Set("Content-Type", type).has_value(), "set form media type");
        auto request = mhttp::detail::ServerRequestAccess::Create(std::move(head));
        auto const body = std::string{ "?field=literal&field=form&field=again&encoded=a%26b%3Dc%2Bd+e%00&flag&=empty-name&bad=%ZZ%&json=%7B%22a%22%3A1%7D" };
        mhttp::detail::ServerRequestAccess::SetBody(request, body);
        auto form = request.Form();
        Check(form.has_value(), "case-insensitive media type accepts optional charset");
        if (!form) { continue; }
        CheckEq(form->Get("?field"), "literal"sv, "initial question mark belongs to form name");
        CheckEq(form->Values("field"), std::vector{ "form"sv, "again"sv }, "duplicate fields preserve order");
        CheckEq(form->Get("encoded"), "a&b=c+d e\0"sv, "split before decoding delimiters, plus, spaces, and NUL");
        Check(form->Has("flag") && form->Get("flag").empty(), "field without equals has empty value");
        CheckEq(form->Get(""), "empty-name"sv, "explicit empty field name");
        CheckEq(form->Get("bad"), "%ZZ%"sv, "malformed escapes follow tolerant query decoding");
        CheckEq(form->Get("json"), "{\"a\":1}"sv, "JSON string remains an ordinary field value");
        form->Set("field", "mutated collection");
        CheckEq(request.Query().Get("field"), "query"sv, "form mutation does not change URL query");
        CheckEq(request.Body(), body, "form access never rewrites body bytes");
        CheckEq(request.Form()->Values("field"), std::vector{ "form"sv, "again"sv }, "repeated access produces independent owned fields");
        retained = std::move(*form);
    }
    CheckEq(retained.Get("?field"), "literal"sv, "parsed data outlives original request");
    CheckEq(mhttp::QueryParams::Parse("?field=query").Get("field"), "query"sv, "query parsing still removes its optional delimiter");
    for (auto type : { "", "text/plain", "application/x-www-form-urlencoded-extra", "multipart/form-data" }) {
        mhttp::detail::RequestHead head{ .method = "POST" };
        if (*type) { Check(head.headers.Set("Content-Type", type).has_value(), "set nonform type"); }
        auto request = mhttp::detail::ServerRequestAccess::Create(std::move(head));
        mhttp::detail::ServerRequestAccess::SetBody(request, "a=1");
        auto form = request.Form();
        Check(!form && form.error() == mhttp::Error::InvalidRequest, "other or missing media type is an explicit form error");
    }
    mhttp::detail::RequestHead head{ .method = "POST" };
    Check(head.headers.Add("Content-Type", "application/x-www-form-urlencoded").has_value(), "first type");
    Check(head.headers.Add("content-type", "application/x-www-form-urlencoded").has_value(), "duplicate type");
    auto ambiguous = mhttp::detail::ServerRequestAccess::Create(std::move(head));
    CheckEq(ambiguous.Form().error(), mhttp::Error::InvalidHeaders, "duplicate media types are rejected");
    mhttp::detail::RequestHead empty_head{ .method = "POST" };
    Check(empty_head.headers.Set("Content-Type", "application/x-www-form-urlencoded").has_value(), "empty form type");
    auto empty = mhttp::detail::ServerRequestAccess::Create(std::move(empty_head));
    Check(empty.Form() && empty.Form()->Empty(), "empty typed body is an empty form");
}

/**
 * @brief Generate independently owned binary field values, including delimiters and repeated names.
 * @param random Deterministic generator retained by the caller.
 * @return Ordered random fields with deliberately repeated and empty keys.
 */
auto random_fields(std::mt19937& random) -> mhttp::QueryParams {
    mhttp::QueryParams result;
    for (unsigned i{}; i < 10; ++i) {
        std::string value;
        for (unsigned j{}, length = random() % 64; j < length; ++j) { value += static_cast<char>(random() % 256); }
        auto name = i % 3 == 0 ? std::string{} : "key ?&=+" + std::to_string(i % 4);
        result.Add(name, value);
    }
    result.Add("json", "{\"escaped\":\"a&b+c=%\\\\\",\"unicode\":\"你好\"}");
    return result;
}

/**
 * @brief Round-trip encoded forms and random URL queries through an actual client and asynchronous server.
 */
auto form_and_query_round_trips() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(4).has_value(), "start form integration runtime");
    mhttp::server::HttpServer server{ runtime };
    Check(server.Post("/form", [](auto& request, auto& response) {
        auto form = request.Form();
        if (!form) {
            Check(response.SetError(mhttp::Status::BadRequest).has_value(), "reject nonform body");
            return;
        }
        CheckEq(request.Query().Get("origin"), "query"sv, "URL query stays independent of form fields");
        Check(response.SetContent(form->Encode(), "application/x-www-form-urlencoded").has_value(), "echo decoded/reencoded form");
    }).has_value(), "register form echo");
    Check(server.Get("/query", [](auto& request, auto& response) {
        Check(response.SetContent(request.Query().Encode(), "text/plain").has_value(), "echo ordered query");
    }).has_value(), "register query echo");
    auto port = server.BindToAnyPort("127.0.0.1");
    Check(port && server.Start() && server.WaitUntilReady(), "start dynamic-port form peer");
    mhttp::client::HttpClient client{ runtime, "127.0.0.1", *port };
    std::mt19937 random{ 0xF04D2026 };
    for (unsigned round{}; round < 15; ++round) {
        auto expected = random_fields(random);
        auto encoded = expected.Encode();
        auto form = mhttp::SyncWait(runtime, client.Post("/form?origin=query", encoded,
            "Application/X-WWW-Form-Urlencoded; charset=UTF-8"));
        Check(form && form->Body() == encoded, "15 random binary form round trips preserve fields, values, and order");
        if (form) { CheckEq(mhttp::QueryParams::ParseForm(form->Body()).Size(), expected.Size(), "form retains repeated and empty fields"); }
        auto query = mhttp::SyncWait(runtime, client.Get("/query", expected));
        Check(query && query->Body() == encoded, "15 random binary queries preserve caller insertion order");
    }
    auto rejected = mhttp::SyncWait(runtime, client.Post("/form?origin=query", "a=1", "text/plain"));
    Check(rejected && rejected->Status() == mhttp::Status::BadRequest, "application maps wrong form media type to 400");
}

/**
 * @brief Run bounded form and query integration scenarios.
 * @param argc Argument count.
 * @param argv Optional test filters.
 * @return Process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "form media and ownership", form_media_and_ownership },
        { "form and query randomized HTTP round trips", form_and_query_round_trips },
    });
}
