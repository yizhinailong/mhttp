/**
 * @file serializer_test.cpp
 * @brief Verify authoritative wire framing, body suppression, and injection rejection.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"
#include "../support/parser_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using mhttp::test::check_all_splits;
using namespace mhttp::detail;
using namespace std::string_view_literals;

/**
 * @brief Verify fixed framing replaces all caller lengths and preserves other fields in order.
 */
auto authoritative_framing() -> void {
    ResponseHead head;
    Check(head.headers.Add("Content-Length", "999").has_value(), "add stale length");
    Check(head.headers.Add("Transfer-Encoding", "gzip").has_value(), "add stale coding");
    Check(head.headers.Add("content-length", "bad").has_value(), "add conflicting stale length");
    Check(head.headers.Add("Trailer", "X-Sum").has_value(), "add stale trailer declaration");
    Check(head.headers.Add("X-A", "1").has_value(), "add first field");
    Check(head.headers.Add("X-B", "2").has_value(), "add second field");
    Check(head.headers.Add("X-A", "3").has_value(), "add third field");
    auto const encoded = serialize_response_head(head, mhttp::Method::Get, { .body_bytes = 3 });
    if (!Check(encoded.has_value(), "response serializes despite stale framing")) { return; }
    CheckEq(encoded->bytes, "HTTP/1.1 200 OK\r\nX-A: 1\r\nX-B: 2\r\nX-A: 3\r\nContent-Length: 3\r\n\r\n"sv,
        "only recomputed framing is emitted and unrelated fields retain order");
    Check(encoded->send_body && encoded->keep_alive, "ordinary HTTP/1.1 response permits body and reuse");
    CheckEq(head.headers.Count("Content-Length"), std::size_t{ 2 }, "serialization does not mutate caller fields");
    std::string const wire = encoded->bytes + std::string{ "a\0b"sv };
    check_all_splits<ResponseParser>(wire, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "serialized response parses at every boundary");
        CheckEq(parsed.body, "a\0b"sv, "binary body length is exact");
        CheckEq(parsed.head.headers.Count("Content-Length"), std::size_t{ 1 }, "single authoritative length on wire");
    });
    auto const empty = serialize_response_head({});
    Check(empty && empty->bytes == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", "empty body has length zero and no inferred type");
    head.status = static_cast<mhttp::Status>(299);
    head.reason = "Custom\tReason";
    auto const custom = serialize_response_head(head);
    Check(custom && custom->bytes.starts_with("HTTP/1.1 299 Custom\tReason\r\n"), "extension status and custom reason are supported");
}

/**
 * @brief Verify status-based suppression and identical GET/HEAD representation headers.
 */
auto body_suppression() -> void {
    for (auto status : { mhttp::Status::Continue, mhttp::Status::EarlyHints,
                         mhttp::Status::NoContent, mhttp::Status::NotModified }) {
        ResponseHead head;
        head.status = status;
        Check(head.headers.Set("Content-Length", "123").has_value(), "set prohibited length");
        Check(head.headers.Set("Transfer-Encoding", "chunked").has_value(), "set prohibited coding");
        Check(head.headers.Set("Trailer", "X-Next").has_value(), "set prohibited trailers");
        for (bool chunked : { false, true }) {
            auto const result = serialize_response_head(head, mhttp::Method::Get,
                { .body_bytes = 123, .chunked = chunked });
            if (!Check(result.has_value(), "bodyless status serializes")) { continue; }
            Check(!result->send_body && result->body_kind == BodyKind::None, "bodyless status forbids wire body");
            Check(result->bytes.find("Content-Length") == std::string::npos
                && result->bytes.find("Transfer-Encoding") == std::string::npos
                && result->bytes.find("Trailer:") == std::string::npos, "bodyless status removes framing and trailer declarations");
        }
    }
    ResponseHead head;
    Check(head.headers.Set("Content-Type", "application/octet-stream").has_value(), "set representation type");
    for (bool chunked : { false, true }) {
        SerializeOptions const options{ .body_bytes = 123, .chunked = chunked };
        auto const get = serialize_response_head(head, mhttp::Method::Get, options);
        auto const read_head = serialize_response_head(head, mhttp::Method::Head, options);
        if (!Check(get && read_head, "GET and HEAD both serialize")) { continue; }
        CheckEq(get->bytes, read_head->bytes, "HEAD headers exactly match GET including fixed or unknown-length chunked framing");
        Check(get->send_body && !read_head->send_body, "only HEAD suppresses body delivery");
    }
}

/**
 * @brief Verify HTTP/1.0 falls back to a known length and Connection close overrides persistence.
 */
auto versions_and_persistence() -> void {
    ResponseHead head;
    head.version = mhttp::Version::Http10;
    auto const old = serialize_response_head(head, mhttp::Method::Get, { .body_bytes = 7, .chunked = true });
    Check(old && old->bytes == "HTTP/1.0 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\n"
        && !old->keep_alive && old->body_kind == BodyKind::ContentLength, "HTTP/1.0 never selects chunked and defaults to closure");
    Check(head.headers.Add("Connection", "KeEp-AlIvE, x-option").has_value(), "enable old persistence");
    auto const persistent = serialize_response_head(head);
    Check(persistent && persistent->keep_alive, "HTTP/1.0 explicit persistence is retained");
    auto const forced = serialize_response_head(head, mhttp::Method::Get, { .keep_alive = false });
    if (Check(forced.has_value(), "forced close serializes")) {
        check_all_splits<ResponseParser>(forced->bytes, [](auto const& parsed) {
            Check(parsed.complete && !parsed.framing.keep_alive, "appended close wins over existing keep-alive");
        });
    }
    head.version = mhttp::Version::Http11;
    Check(head.headers.Add("Connection", "close").has_value(), "add later close token");
    auto const closed = serialize_response_head(head);
    Check(closed && !closed->keep_alive, "handler-requested closure cannot be overridden by default policy");
    head.version = static_cast<mhttp::Version>(9);
    Check(serialize_response_head(head).error() == mhttp::Error::InvalidRequest, "invalid version rejected");
}

/**
 * @brief Verify outbound methods, Host, and target bytes before a transport receives a head.
 */
auto request_validation() -> void {
    RequestHead head;
    Check(serialize_request_head(head).error() == mhttp::Error::InvalidHeaders, "HTTP/1.1 requires Host before serialization");
    Check(head.headers.Set("Host", "[::1]:8080").has_value(), "set valid IPv6 authority");
    head.method = "POST";
    head.target = "/a b\r\n?q=%2F+z&x=1";
    head.target.push_back('\0');
    head.target += "\t\x7f";
    Check(head.headers.Set("Content-Length", "bad").has_value(), "set stale request length");
    Check(head.headers.Set("Transfer-Encoding", "chunked").has_value(), "set stale request transfer coding");
    auto const encoded = serialize_request_head(head, { .body_bytes = 3 });
    if (!Check(encoded.has_value(), "control bytes are encoded before output")) { return; }
    Check(encoded->bytes.starts_with("POST /a%20b%0D%0A?q=%2F+z&x=1%00%09%7F HTTP/1.1\r\n"),
        "spaces and controls encoded while percent escapes and query punctuation retain spelling");
    check_all_splits<RequestParser>(encoded->bytes + "abc", [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure && parsed.body == "abc", "serialized request parses without splitting");
        CheckEq(parsed.head.target, "/a%20b%0D%0A?q=%2F+z&x=1%00%09%7F"sv, "encoded target survives parser unchanged");
    });
    for (auto method : { "", "G ET", "GET\r\nInjected: yes", "GET\t" }) {
        head.method = method;
        Check(serialize_request_head(head).error() == mhttp::Error::InvalidRequest, "invalid method cannot produce a head");
    }
    head.method = "OPTIONS";
    head.target = "*";
    Check(serialize_request_head(head).has_value(), "OPTIONS asterisk permitted");
    head.method = "GET";
    for (auto target : { "", "*", "relative", "/path#fragment", "http://example.com/" }) {
        head.target = target;
        Check(serialize_request_head(head).error() == mhttp::Error::InvalidRequest, "non-origin or fragment target rejected");
    }
    head.target = "/";
    Check(head.headers.Add("Host", "[::1]:8080").has_value(), "add duplicate authority");
    Check(serialize_request_head(head).error() == mhttp::Error::InvalidHeaders, "duplicate Host rejected before output");
    Check(head.headers.Set("Host", "evil@good").has_value(), "set lexically valid but ambiguous authority");
    Check(serialize_request_head(head).error() == mhttp::Error::InvalidHeaders, "ambiguous Host rejected before output");
}

/**
 * @brief Verify rejected header mutations never place CRLF or NUL on the wire.
 */
auto splitting_rejection() -> void {
    ResponseHead response;
    RequestHead request;
    Check(request.headers.Set("Host", "localhost").has_value(), "set valid request authority");
    for (auto* fields : { &response.headers, &request.headers }) {
        Check(fields->Set("X-Safe", "old").has_value(), "set safe original value");
        for (auto value : { "a\r\nInjected: yes"sv, "a\nb"sv, "a\rb"sv, "a\0b"sv }) {
            auto const rejected = fields->Set("X-Safe", value);
            Check(!rejected && rejected.error() == mhttp::Error::InvalidHeaders, "invalid header returns error at mutation boundary");
            CheckEq(fields->Get("X-Safe"), "old"sv, "rejected mutation preserves original value");
        }
        Check(!fields->Add("X\r\nInjected", "yes"), "invalid field name rejected");
    }
    auto const encoded_response = serialize_response_head(response);
    auto const encoded_request = serialize_request_head(request);
    Check(encoded_response && encoded_response->bytes.find("Injected") == std::string::npos,
        "response cannot contain rejected field bytes");
    Check(encoded_request && encoded_request->bytes.find("Injected") == std::string::npos,
        "request cannot contain rejected field bytes");
    for (auto reason : { "OK\r\nInjected: yes"sv, "a\0b"sv, "a\nb"sv, "a\x7f"sv }) {
        response.reason = reason;
        auto const rejected = serialize_response_head(response);
        Check(!rejected && rejected.error() == mhttp::Error::InvalidHeaders, "reason injection rejected before producing output");
    }
    response.reason.clear();
    for (auto code : { 0, 99, 1000 }) {
        response.status = static_cast<mhttp::Status>(code);
        Check(serialize_response_head(response).error() == mhttp::Error::InvalidRequest, "non-three-digit status rejected");
    }
}

/**
 * @brief Run serializer cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "authoritative framing", authoritative_framing },
        { "body suppression", body_suppression },
        { "versions and persistence", versions_and_persistence },
        { "request validation", request_validation },
        { "splitting rejection", splitting_rejection },
    });
}
