/**
 * @file request_policy_test.cpp
 * @brief Verify client origins, query composition, fallback fields, framing, and pre-I/O admission bounds.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/parser_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace mhttp::detail;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

/**
 * @brief Require safe positive deadlines/limits while allowing empty response bodies and no built-in agent.
 */
auto bounded_configuration() -> void {
    mhttp::client::ClientOptions options;
    Check(validate_client_options(options).has_value(), "defaults are valid");
    CheckEq(options.user_agent, "mhttp/" + std::string{ mhttp::VERSION }, "agent identifies the library release");
    for (auto member : { &mhttp::client::ClientOptions::connect_timeout,
                        &mhttp::client::ClientOptions::read_timeout, &mhttp::client::ClientOptions::write_timeout,
                        &mhttp::client::ClientOptions::timeout }) {
        for (auto duration : { 0ms, -1ms, std::chrono::milliseconds::max() }) {
            auto changed = options;
            changed.*member = duration;
            CheckEq(validate_client_options(changed).error(), mhttp::Error::InvalidRequest, "reject unsafe deadline");
        }
    }
    auto invalid_policy = options;
    invalid_policy.timeout_policy = static_cast<mhttp::client::TimeoutPolicy>(99);
    CheckEq(validate_client_options(invalid_policy).error(), mhttp::Error::InvalidRequest, "reject unknown timeout policy");
    for (auto member : { &mhttp::client::ClientOptions::header_bytes, &mhttp::client::ClientOptions::header_fields,
                        &mhttp::client::ClientOptions::max_request_bytes }) {
        auto changed = options;
        changed.*member = 0;
        CheckEq(validate_client_options(changed).error(), mhttp::Error::InvalidRequest, "reject zero admission bound");
    }
    for (auto agent : { "a\r\nb", " a", "a ", "a\x7f" }) {
        options.user_agent = agent;
        CheckEq(validate_client_options(options).error(), mhttp::Error::InvalidHeaders, "reject unsafe default agent");
    }
    options.user_agent.clear();
    options.body_bytes = 0;
    options.header_bytes = 120;
    options.header_fields = 3;
    Check(validate_client_options(options).has_value(), "empty-only response and no built-in agent are valid");
    auto limits = client_parser_limits(options);
    CheckEq(limits.body_bytes, std::uint64_t{ 0 }, "empty response bound reaches parser");
    CheckEq(limits.header_bytes, std::size_t{ 120 }, "header byte bound reaches parser");
    CheckEq(limits.header_fields, std::size_t{ 3 }, "header count bound reaches parser");
}

/**
 * @brief Validate constructor hosts without accepting embedded URLs, userinfo, paths, or injected authorities.
 */
auto constructor_origins() -> void {
    constexpr std::pair<std::string_view, std::string_view> CASES[]{
        { "Example.COM", "example.com:8080" }, { "127.0.0.1", "127.0.0.1:8080" },
        { "::1", "[::1]:8080" }, { "[::1]", "[::1]:8080" }, { "2001:DB8::1", "[2001:db8::1]:8080" },
    };
    for (auto const& [host, authority] : CASES) {
        auto origin = client_origin(host, 8080);
        if (!Check(origin.has_value(), std::format("valid constructor host {}", host))) { continue; }
        CheckEq(mhttp::MakeHostValue(origin->Host(), origin->Port()), authority, "normalize host and bracket IPv6");
    }
    for (auto host : { "", " ", "\t", "http://host", "host/path", "host?x", "host#f", "host:80", "host@other",
                       "[]", "[host]", "[::1]:80", "1::2::3", "[::1", "::1]", "host\r\nInjected:yes", "host%2Fpath" }) {
        CheckEq(client_origin(host, 80).error(), mhttp::Error::InvalidUrl, std::format("reject constructor host {:?}", host));
    }
    CheckEq(client_origin("a\0b"sv, 80).error(), mhttp::Error::InvalidUrl, "reject embedded host NUL");
    auto origin = client_origin("localhost", 80);
    CheckEq(mhttp::MakeHostValue(origin->Host(), origin->Port()), "localhost"sv, "default HTTP port is omitted");
    Check(client_origin("localhost", 0).has_value(), "port zero is syntactically valid without opening a socket");
    CheckEq(mhttp::Url::Parse("https://host/").error(), mhttp::Error::UnsupportedScheme, "URL constructor rejects TLS scheme");
}

/**
 * @brief Preserve existing escapes and query order, encode arbitrary added bytes, and remove local fragments.
 */
auto query_composition() -> void {
    auto origin = *mhttp::Url::Parse("http://host/base?from=url#base-fragment");
    mhttp::client::Request request{ mhttp::Method::Get, "/a b%2Fc?existing=1#local?ignored=yes" };
    request.Query().Add("tag", "one two");
    request.Query().Add("tag", "a/+%");
    request.Query().Add("", "x\0y"sv);
    auto prepared = prepare_client_request(std::move(request), origin);
    if (!Check(prepared.has_value(), "compose query before serialization")) { return; }
    auto const expected = "/a%20b%2Fc?existing=1&tag=one+two&tag=a%2F%2B%25&=x%00y"sv;
    CheckEq(prepared->target, expected, "raw escapes survive and additional repeated/binary values are encoded once");
    mhttp::test::check_all_splits<RequestParser>(prepared->head.bytes, [expected](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "prepared target parses at every boundary");
        CheckEq(parsed.head.target, expected, "peer receives the exact composed target");
    });
    constexpr std::pair<std::string_view, std::string_view> CASES[]{
        { "", "/base?from=url&k=v" }, { "#local", "/base?from=url&k=v" },
        { "/path?", "/path?k=v" }, { "/path?x=1&", "/path?x=1&k=v" },
        { "/path?x=?", "/path?x=?&k=v" }, { "/path", "/path?k=v" },
    };
    for (auto const& [target, expected_target] : CASES) {
        mhttp::client::Request next{ mhttp::Method::Get, std::string{ target } };
        next.Query().Add("k", "v");
        auto result = prepare_client_request(std::move(next), origin);
        if (Check(result.has_value(), "compose base or explicit target")) {
            CheckEq(result->target, expected_target, "append the correct query separator");
        }
    }
    mhttp::client::Request controls{ mhttp::Method::Get, std::string{ "/x\r\n\t\0y"sv } };
    auto encoded = prepare_client_request(std::move(controls), origin);
    Check(encoded && encoded->target == "/x%0D%0A%09%00y", "target controls cannot inject another request line");
}

/**
 * @brief Preserve explicit duplicate fields and all matching default values while applying fallback precedence.
 */
auto default_fields() -> void {
    auto origin = *client_origin("::1", 8080);
    mhttp::client::ClientOptions options;
    Check(options.default_headers.Add("X-Default", "one").has_value(), "first default");
    Check(options.default_headers.Add("x-default", "two").has_value(), "second default");
    Check(options.default_headers.Add("X-Explicit", "ignored").has_value(), "overridden default");
    Check(options.default_headers.Set("User-Agent", "custom-default").has_value(), "default agent overrides built-in");
    mhttp::client::Request request;
    Check(request.Headers().Add("x-explicit", "first").has_value(), "explicit first value");
    Check(request.Headers().Add("X-Explicit", "second").has_value(), "explicit second value");
    auto prepared = prepare_client_request(std::move(request), origin, options);
    if (!Check(prepared.has_value(), "apply default fields")) { return; }
    auto parsed = mhttp::test::parse_wire<RequestParser>(prepared->head.bytes, std::array{ prepared->head.bytes.size() });
    Check(parsed.complete && !parsed.failure, "default fields produce a valid complete request");
    CheckEq(parsed.head.headers.Values("X-Default"), std::vector{ "one"sv, "two"sv }, "preserve duplicate default values");
    CheckEq(parsed.head.headers.Values("X-Explicit"), std::vector{ "first"sv, "second"sv }, "explicit name suppresses every default value");
    CheckEq(parsed.head.headers.Get("Host"), "[::1]:8080"sv, "auto Host uses destination IPv6 and nondefault port");
    CheckEq(parsed.head.headers.Get("User-Agent"), "custom-default"sv, "default field overrides built-in user agent");
    Check(!parsed.head.headers.Has("Accept-Encoding") && !parsed.head.headers.Has("Content-Type"), "infer neither decoder support nor media type");
    CheckEq(options.default_headers.Count("X-Default"), std::size_t{ 2 }, "preparation does not consume configuration fields");
    mhttp::client::Request explicit_fields;
    Check(explicit_fields.Headers().Set("Host", "virtual.example").has_value(), "explicit virtual Host");
    Check(explicit_fields.Headers().Set("User-Agent", "").has_value(), "explicit empty agent");
    auto explicit_result = prepare_client_request(std::move(explicit_fields), origin, options);
    Check(explicit_result && explicit_result->head.bytes.contains("Host: virtual.example\r\n")
        && explicit_result->head.bytes.contains("User-Agent: \r\n")
        && !explicit_result->head.bytes.contains("custom-default"), "explicit Host and empty agent both win");
    auto built_in = prepare_client_request(mhttp::client::Request{}, *client_origin("host", 80));
    Check(built_in && built_in->head.bytes.contains("Host: host\r\n")
        && built_in->head.bytes.contains("User-Agent: mhttp/" + std::string{ mhttp::VERSION } + "\r\n"), "built-in defaults identify origin and release");
    options.default_headers.Clear();
    options.user_agent.clear();
    auto no_agent = prepare_client_request(mhttp::client::Request{}, origin, options);
    Check(no_agent && !no_agent->head.bytes.contains("User-Agent:"), "empty configured agent suppresses built-in field");
}

/**
 * @brief Keep owned binary body storage and replace every stale framing field using the actual representation.
 */
auto body_and_connection_policy() -> void {
    auto origin = *client_origin("host", 80);
    mhttp::client::Request request{ mhttp::Method::Post, "/upload" };
    Check(request.SetContent(std::string{ "a\0b"sv }, "application/octet-stream").has_value(), "binary request content");
    auto const* body_storage = request.Body().data();
    Check(request.Headers().Add("Content-Length", "invalid").has_value(), "stale length");
    Check(request.Headers().Add("Transfer-Encoding", "chunked").has_value(), "stale transfer coding");
    Check(request.Headers().Add("Trailer", "X-Fake").has_value(), "stale trailers");
    auto prepared = prepare_client_request(std::move(request), origin);
    if (!Check(prepared.has_value(), "prepare owned binary request")) { return; }
    CheckEq(prepared->request.Body().data(), body_storage, "request retains its original body storage without copying");
    CheckEq(prepared->request.Body(), "a\0b"sv, "body remains independent of head storage");
    Check(prepared->head.keep_alive && prepared->head.send_body, "default request permits body and connection reuse");
    mhttp::test::check_all_splits<RequestParser>(prepared->head.bytes + prepared->request.Body(), [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "prepared body parses at every boundary");
        CheckEq(parsed.body, "a\0b"sv, "binary representation is framed exactly");
        CheckEq(parsed.head.method, "POST"sv, "body retains its originating method");
        CheckEq(parsed.head.headers.Get("Content-Length"), "3"sv, "actual body length replaces stale values");
        Check(!parsed.head.headers.Has("Transfer-Encoding") && !parsed.head.headers.Has("Trailer"), "buffered requests contain no stale streaming framing");
    });
    mhttp::client::ClientOptions options{ .keep_alive = false };
    auto close = prepare_client_request(mhttp::client::Request{}, origin, options);
    Check(close && !close->head.keep_alive && close->head.bytes.contains("Connection: close\r\n"), "configuration can disable persistence");
    mhttp::client::Request closing;
    Check(closing.Headers().Set("Connection", "keep-alive, close").has_value(), "caller selects close");
    auto explicit_close = prepare_client_request(std::move(closing), origin);
    Check(explicit_close && !explicit_close->head.keep_alive, "caller close wins over configured persistence");
}

/**
 * @brief Reject invalid complete requests and enforce the exact total head-plus-body bound before transmission.
 */
auto request_admission() -> void {
    auto origin = *client_origin("host", 80);
    for (auto target : { "relative", "http://other/", "?query", "*" }) {
        auto result = prepare_client_request(mhttp::client::Request{ mhttp::Method::Get, target }, origin);
        CheckEq(result.error(), mhttp::Error::InvalidRequest, "non-origin GET target fails before I/O");
    }
    auto star = prepare_client_request(mhttp::client::Request{ mhttp::Method::Options, "*#local" }, origin);
    Check(star && star->head.bytes.starts_with("OPTIONS * HTTP/1.1\r\n"), "OPTIONS asterisk is supported");
    auto method = prepare_client_request(mhttp::client::Request{ "GET\r\nINJECT", "/" }, origin);
    CheckEq(method.error(), mhttp::Error::InvalidRequest, "raw constructor method is validated at the send boundary");
    auto extension = prepare_client_request(mhttp::client::Request{ "CUSTOM", "/" }, origin);
    Check(extension && extension->head.bytes.starts_with("CUSTOM / HTTP/1.1\r\n"), "valid extension tokens can be sent to peers that support them");
    mhttp::client::Request duplicate_host;
    Check(duplicate_host.Headers().Add("Host", "one").has_value(), "first host");
    Check(duplicate_host.Headers().Add("host", "two").has_value(), "second host");
    CheckEq(prepare_client_request(std::move(duplicate_host), origin).error(), mhttp::Error::InvalidHeaders, "duplicate Host is never sent");
    mhttp::client::ClientOptions invalid_host;
    Check(invalid_host.default_headers.Set("Host", "bad:authority:80").has_value(), "lexically safe but invalid default Host");
    CheckEq(prepare_client_request(mhttp::client::Request{}, origin, invalid_host).error(), mhttp::Error::InvalidHeaders, "default Host also receives semantic validation");
    auto baseline = prepare_client_request(mhttp::client::Request{}, origin);
    mhttp::client::ClientOptions limited{ .max_request_bytes = baseline->head.bytes.size() };
    Check(prepare_client_request(mhttp::client::Request{}, origin, limited).has_value(), "exact complete wire bound is accepted");
    --limited.max_request_bytes;
    CheckEq(prepare_client_request(mhttp::client::Request{}, origin, limited).error(), mhttp::Error::BodyTooLarge, "even an empty body requires room for its complete head");
    mhttp::client::Request too_large;
    Check(too_large.SetContent(std::string(limited.max_request_bytes + 1, 'x')).has_value(), "oversized owned request body");
    CheckEq(prepare_client_request(std::move(too_large), origin, limited).error(), mhttp::Error::BodyTooLarge, "oversized body is rejected before head generation");
}

/**
 * @brief Run client preparation scenarios with bounded per-case watchdogs.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Process test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client bounded configuration", bounded_configuration },
        { "client constructor origins", constructor_origins },
        { "client query composition", query_composition },
        { "client fallback fields", default_fields },
        { "client body and connection policy", body_and_connection_policy },
        { "client request admission", request_admission },
    });
}
