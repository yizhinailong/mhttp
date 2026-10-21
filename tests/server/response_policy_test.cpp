/**
 * @file response_policy_test.cpp
 * @brief Verify automatic response fields, lifecycle persistence, wire bounds, and configured parser admission.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using mhttp::detail::ServerRequestAccess;
using mhttp::detail::serialize_server_response;
using namespace std::chrono_literals;
using namespace std::string_view_literals;

/**
 * @brief Parse a complete generated response to verify field repetition and binary body framing.
 * @param wire Owned wire bytes borrowed until parsing finishes.
 * @param method Originating request method.
 * @return Owned parsed head and body.
 */
auto decode(
    std::string const& wire,
    mhttp::Method method = mhttp::Method::Get
) -> std::pair<mhttp::detail::ResponseHead, std::string> {
    mhttp::detail::ResponseParser parser{ method };
    std::span<char const> remaining{ wire.data(), wire.size() };
    std::string body;
    for (;;) {
        auto step = parser.Feed(remaining);
        remaining = remaining.subspan(step.consumed);
        if (step.event == mhttp::detail::ParseEvent::Body) { body.append(step.data.data(), step.data.size()); }
        if (step.event == mhttp::detail::ParseEvent::Complete) { break; }
        if (step.event == mhttp::detail::ParseEvent::Failed || step.event == mhttp::detail::ParseEvent::NeedMore) {
            Check(false, "generated complete response must parse without failure or missing bytes");
            break;
        }
    }
    Check(remaining.empty(), "generated response contains exactly one message");
    return { parser.Head(), std::move(body) };
}

/**
 * @brief Supply a deterministic automatic Date and preserve explicit fields and repeated fallback values.
 */
auto response_automatic_fields() -> void {
    auto request = ServerRequestAccess::Create({});
    mhttp::server::Response response;
    mhttp::server::ServerOptions options;
    Check(options.default_headers.Add("X-Tag", "first").has_value(), "first repeated fallback");
    Check(options.default_headers.Add("x-tag", "second").has_value(), "second repeated fallback");
    Check(options.default_headers.Add("X-Override", "default").has_value(), "overridden fallback");
    Check(response.Headers().Set("x-override", "application").has_value(), "explicit response field");
    Check(response.Headers().Set("Content-Length", "999").has_value(), "stale framing field");
    Check(response.SetContent(std::string{ "a\0b"sv }, "application/octet-stream").has_value(), "binary representation");
    auto now = std::chrono::sys_days{ std::chrono::year{ 1994 } / 11 / 6 } + 8h + 49min + 37s;
    auto wire = serialize_server_response(request, response, options, {}, {}, now);
    if (!Check(wire.has_value(), "prepare automatic response fields")) { return; }
    auto [head, body] = decode(wire->bytes);
    CheckEq(head.headers.Get("Date"), "Sun, 06 Nov 1994 08:49:37 GMT"sv, "automatic Date uses supplied instant");
    CheckEq(head.headers.Values("X-Tag"), std::vector{ "first"sv, "second"sv }, "all repeated defaults retained");
    CheckEq(head.headers.Values("X-Override"), std::vector{ "application"sv }, "application fields replace defaults as a group");
    CheckEq(head.headers.Values("Content-Length"), std::vector{ "3"sv }, "one authoritative content length");
    CheckEq(body, "a\0b"sv, "binary wire body preserved");
    Check(wire->keep_alive, "default first response permits persistence");
    Check(!response.Headers().Has("Date") && !response.Headers().Has("X-Tag"), "serialization leaves application fields unchanged");
    CheckEq(response.Header("Content-Length"), "999"sv, "application response snapshot remains independent");
    Check(response.Headers().Set("Date", "Mon, 01 Jan 2024 00:00:00 GMT").has_value(), "explicit Date");
    auto explicit_date = serialize_server_response(request, response, options, {}, {}, now);
    Check(explicit_date && explicit_date->bytes.contains("Date: Mon, 01 Jan 2024 00:00:00 GMT\r\n"), "explicit Date preserved");
}

/**
 * @brief Every lifecycle constraint independently overrides keep-alive preferences on either side.
 */
auto response_persistence_policy() -> void {
    for (unsigned scenario{}; scenario < 7; ++scenario) {
        mhttp::detail::RequestHead head;
        mhttp::server::Response response;
        mhttp::server::ServerOptions options;
        mhttp::detail::RoutingOutcome routing;
        mhttp::detail::ResponseSessionState session;
        if (scenario == 0) { session.completed_requests = options.keep_alive_max_requests; }
        if (scenario == 1) { session.body_complete = false; }
        if (scenario == 2) { session.stopping = true; }
        if (scenario == 3) {
            routing.close = true;
            Check(response.Headers().Set("Connection", "keep-alive").has_value(), "application asks to keep alive after failure");
        }
        if (scenario == 4) { Check(head.headers.Set("Connection", "close").has_value(), "peer asks to close"); }
        if (scenario == 5) { Check(response.Headers().Set("Connection", "close").has_value(), "application asks to close"); }
        if (scenario == 6) { Check(options.default_headers.Set("Connection", "close").has_value(), "default asks to close"); }
        auto request = ServerRequestAccess::Create(std::move(head));
        auto wire = serialize_server_response(request, response, options, routing, session);
        Check(wire && !wire->keep_alive && wire->bytes.contains("Connection: close\r\n"), "any closing policy prevents connection reuse");
    }
    mhttp::detail::RequestHead head{ .version = mhttp::Version::Http10 };
    Check(head.headers.Set("Connection", "keep-alive").has_value(), "HTTP/1.0 peer persistence token");
    auto request = ServerRequestAccess::Create(std::move(head));
    mhttp::server::Response response;
    mhttp::server::ServerOptions options;
    auto first = serialize_server_response(request, response, options);
    Check(first && first->keep_alive && first->bytes.starts_with("HTTP/1.0 200"), "response follows request version");
    options.keep_alive_max_requests = 1;
    auto last = serialize_server_response(request, response, options);
    Check(last && !last->keep_alive, "maximum one request disables persistence immediately");
}

/**
 * @brief Apply the response bound to actual wire bytes after HEAD/204/304 suppression.
 */
auto response_wire_bounds() -> void {
    auto request = ServerRequestAccess::Create({});
    mhttp::server::Response response;
    Check(response.SetContent(std::string(4096, 'x'), "text/plain").has_value(), "large representation");
    mhttp::server::ServerOptions options{ .max_response_bytes = 256 };
    auto too_large = serialize_server_response(request, response, options);
    Check(!too_large && too_large.error() == mhttp::Error::BodyTooLarge, "oversized wire response rejected before sending");
    auto head_request = ServerRequestAccess::Create({ .method = "HEAD" });
    auto head_only = serialize_server_response(head_request, response, options);
    Check(head_only && head_only->bytes.ends_with("\r\n\r\n") && head_only->bytes.contains("Content-Length: 4096\r\n"),
        "HEAD retains representation length but budgets only header bytes");
    for (auto status : { mhttp::Status::NoContent, mhttp::Status::NotModified }) {
        Check(response.SetStatus(status).has_value(), "bodyless status");
        auto bodyless = serialize_server_response(request, response, options);
        Check(bodyless && bodyless->bytes.ends_with("\r\n\r\n") && !bodyless->bytes.contains("Content-Length"),
            "204 and 304 fit bound without body or length fields");
    }
    Check(response.SetEmpty(mhttp::Status::Ok).has_value(), "empty response for exact boundary");
    auto small = serialize_server_response(request, response, options);
    if (!Check(small.has_value(), "prepare small response")) { return; }
    options.max_response_bytes = small->bytes.size();
    Check(serialize_server_response(request, response, options).has_value(), "exact wire bound accepted");
    --options.max_response_bytes;
    auto over = serialize_server_response(request, response, options);
    Check(!over && over.error() == mhttp::Error::BodyTooLarge, "one-byte overflow rejected");
    options.max_response_bytes = 256;
    Check(response.Headers().Set("X-Large", std::string(300, 'x')).has_value(), "large response field");
    Check(!serialize_server_response(request, response, options), "header bytes also count toward output bound");
}

/**
 * @brief Refuse an incomplete final response contract and reject invalid resource/deadline configuration.
 */
auto response_rejected_configuration() -> void {
    auto request = ServerRequestAccess::Create({});
    mhttp::server::Response response;
    mhttp::server::ServerOptions options;
    auto failed_post = serialize_server_response(request, response, options, { .send_response = false });
    Check(!failed_post && failed_post.error() == mhttp::Error::InvalidRequest, "failed post-routing cannot serialize partial output");
    Check(!serialize_server_response(request, response, options, {}, { .completed_requests = 0 }), "session count starts at one");
    Check(response.SetStatus(mhttp::Status::Continue).has_value(), "informational metadata");
    Check(!serialize_server_response(request, response, options), "100 cannot be the sole final buffered response");
    Check(response.SetEmpty(mhttp::Status::Ok).has_value(), "restore final status");
    for (unsigned scenario{}; scenario < 10; ++scenario) {
        options = {};
        switch (scenario) {
        case 0: options.read_timeout = 0ms; break;
        case 1: options.write_timeout = -1ms; break;
        case 2: options.keep_alive_timeout = 0ms; break;
        case 3: options.keep_alive_max_requests = 0; break;
        case 4: options.uri_bytes = 0; break;
        case 5: options.header_bytes = 0; break;
        case 6: options.header_fields = 0; break;
        case 7: options.max_response_bytes = 0; break;
        case 8: options.read_timeout = std::chrono::milliseconds::max(); break;
        case 9: options.max_connections = 0; break;
        }
        auto invalid = serialize_server_response(request, response, options);
        Check(!invalid && invalid.error() == mhttp::Error::InvalidRequest, "invalid options rejected before output allocation");
    }
}

/**
 * @brief Feed requests through limits derived from server configuration, including deliberate no-body admission.
 */
auto server_parser_configuration() -> void {
    std::vector<std::pair<mhttp::server::ServerOptions, std::string>> inputs{
        { { .uri_bytes = 3 }, "GET /long HTTP/1.1\r\nHost: localhost\r\n\r\n" },
        { { .header_fields = 1 }, "GET / HTTP/1.1\r\nHost: localhost\r\nX-Extra: yes\r\n\r\n" },
        { { .body_bytes = 0 }, "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n\r\nx" },
    };
    std::array expected{ mhttp::Status::UriTooLong, mhttp::Status::RequestHeaderFieldsTooLarge, mhttp::Status::ContentTooLarge };
    for (std::size_t index{}; index < inputs.size(); ++index) {
        auto const& [options, input] = inputs[index];
        Check(mhttp::detail::validate_server_options(options).has_value(), "small bounds are valid configuration");
        mhttp::detail::RequestParser parser{ mhttp::detail::server_parser_limits(options) };
        auto step = parser.Feed(std::span<char const>{ input.data(), input.size() });
        Check(step.event == mhttp::detail::ParseEvent::Failed && step.failure && step.failure->status == expected[index],
            "configured parser reports the appropriate final server error status");
    }
}

/**
 * @brief Prepare deferred streams without invoking producers or inventing buffered body lengths.
 */
auto response_stream_framing() -> void {
    mhttp::server::Response response;
    Check(response.SetChunkedContentProvider([](auto&) -> mhttp::Task<mhttp::Result<void>> {
        Check(false, "head preparation never invokes producer code");
        co_return mhttp::Result<void>{};
    }).has_value(), "configure deferred stream");
    Check(response.Headers().Add("Content-Length", "999").has_value(), "stale stream length");
    Check(response.Headers().Add("Transfer-Encoding", "gzip").has_value(), "stale stream coding");
    for (auto version : { mhttp::Version::Http10, mhttp::Version::Http11 }) {
        for (auto method : { "GET", "HEAD" }) {
            auto request = ServerRequestAccess::Create({ .method = method, .version = version });
            auto wire = serialize_server_response(request, response, {});
            if (!Check(wire.has_value(), "stream head serializes")) { continue; }
            CheckEq(wire->streaming, std::string_view{ method } == "GET", "HEAD skips the producer");
            Check(wire->bytes.ends_with("\r\n\r\n") && !wire->bytes.contains("Content-Length:"), "no aggregate body length invented");
            CheckEq(wire->body_kind, version == mhttp::Version::Http11
                ? mhttp::detail::BodyKind::Chunked : mhttp::detail::BodyKind::UntilEof, "wire-version streaming mode");
            CheckEq(wire->keep_alive, version == mhttp::Version::Http11, "only chunked streams may persist");
            if (version == mhttp::Version::Http10) {
                Check(!wire->bytes.contains("Connection: keep-alive\r\n"), "EOF stream never auto-advertises persistence");
            }
            CheckEq(wire->bytes.contains("Transfer-Encoding: chunked\r\n"), version == mhttp::Version::Http11,
                "chunked coding is exclusive to HTTP/1.1");
            mhttp::server::ServerOptions exact{ .max_response_bytes = wire->bytes.size() };
            Check(serialize_server_response(request, response, exact).has_value(), "stream head fits exact bound");
            --exact.max_response_bytes;
            CheckEq(serialize_server_response(request, response, exact).error(), mhttp::Error::BodyTooLarge,
                "stream head bound applies before commitment");
        }
    }
    for (auto status : { mhttp::Status::NoContent, mhttp::Status::NotModified }) {
        Check(response.SetStatus(status).has_value(), "bodyless streaming status");
        auto request = ServerRequestAccess::Create({});
        auto wire = serialize_server_response(request, response, {});
        Check(wire && !wire->streaming && wire->body_kind == mhttp::detail::BodyKind::None
            && !wire->bytes.contains("Transfer-Encoding:"), "bodyless status skips provider and terminal framing");
        mhttp::detail::RequestHead old_head{ .version = mhttp::Version::Http10 };
        Check(old_head.headers.Add("Connection", "keep-alive").has_value(), "HTTP/1.0 bodyless persistence request");
        auto old_request = ServerRequestAccess::Create(std::move(old_head));
        auto old = serialize_server_response(old_request, response, {});
        Check(old && old->keep_alive && !old->streaming && old->bytes.contains("Connection: keep-alive\r\n"),
            "HTTP/1.0 bodyless responses still permit explicit persistence without running the provider");
    }
    Check(response.SetEmpty(mhttp::Status::Ok).has_value(), "restore body status");
    Check(response.SetContentProvider(1048576, [](auto&) -> mhttp::Task<mhttp::Result<void>> {
        Check(false, "fixed source never runs during preparation");
        co_return mhttp::Result<void>{};
    }).has_value(), "configure fixed stream");
    for (auto version : { mhttp::Version::Http10, mhttp::Version::Http11 }) {
        mhttp::detail::RequestHead head{ .method = "GET", .version = version };
        static_cast<void>(head.headers.Set("Connection", "keep-alive"));
        auto request = ServerRequestAccess::Create(std::move(head));
        auto wire = serialize_server_response(request, response, { .max_response_bytes = 512 });
        Check(wire && wire->streaming && wire->keep_alive && wire->content_length == 1048576
            && wire->body_kind == mhttp::detail::BodyKind::ContentLength
            && wire->bytes.contains("Content-Length: 1048576\r\n") && !wire->bytes.contains("Transfer-Encoding:"),
            "both HTTP versions frame exact streams and retain persistence without aggregate wire bounds");
    }
    Check(!mhttp::detail::validate_server_options({ .stream_chunk_bytes = 0 }), "zero chunk bound rejected before bind");
}

/**
 * @brief Run bounded response preparation scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "response automatic fields", response_automatic_fields },
        { "response session persistence policy", response_persistence_policy },
        { "response complete wire bounds", response_wire_bounds },
        { "response streaming wire policy", response_stream_framing },
        { "response rejected configuration", response_rejected_configuration },
        { "server configured parser admission", server_parser_configuration },
    });
}
