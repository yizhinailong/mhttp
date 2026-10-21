/**
 * @file request_parser_test.cpp
 * @brief Verify strict incrementally delivered requests, bounded bodies, and anti-smuggling rules.
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
 * @brief Verify legal request forms and ordered, OWS-trimmed fields at every split.
 */
auto request_lines_and_headers() -> void {
    constexpr std::pair<std::string_view, std::string_view> CASES[]{
        { "GET / HTTP/1.1\r\nHost: x\r\n\r\n", "/" },
        { "GET /path?z=2&a=1 HTTP/1.1\r\nhOsT: x\r\n\r\n", "/path?z=2&a=1" },
        { "OPTIONS * HTTP/1.1\r\nHost: x\r\n\r\n", "*" },
        { "GET / HTTP/1.0\r\n\r\n", "/" },
        { "\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n", "/" },
        { "GET /\xE4\xB8\xAD HTTP/1.1\r\nHost: x\r\n\r\n", "/\xE4\xB8\xAD" },
        { "GET http://example.com:8080?q=1 HTTP/1.1\r\nHost: other\r\n\r\n", "/?q=1" },
    };
    for (auto const& [wire, target] : CASES) {
        check_all_splits<RequestParser>(wire, [target, wire](auto const& parsed) {
            Check(parsed.complete && !parsed.failure, std::format("complete request {:?}", wire));
            CheckEq(parsed.head.target, target, "request target is origin-form or OPTIONS asterisk");
            CheckEq(parsed.header_events, std::size_t{ 1 }, "one header-complete event");
            CheckEq(parsed.consumed, wire.size(), "consume exactly one request");
            if (parsed.head.absolute_authority) {
                CheckEq(parsed.head.headers.Get("Host"), "example.com:8080"sv,
                    "absolute-form authority overrides the received Host value");
            }
        });
    }
    constexpr auto ORDERED = "GET / HTTP/1.1\r\nHost: x\r\nX-A: one\r\nX-B:\t \r\nx-a: \ttwo \t\r\nX-Raw: %0D%0A\r\n\r\n"sv;
    check_all_splits<RequestParser>(ORDERED, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "ordered header request completes");
        CheckEq(parsed.head.headers.Values("X-A"), std::vector{ "one"sv, "two"sv }, "repeated header order");
        Check(parsed.head.headers.Has("X-B") && parsed.head.headers.Get("X-B").empty(), "empty field is legal");
        CheckEq(parsed.head.headers.Get("X-Raw"), "%0D%0A"sv, "header values are not percent decoded");
        std::vector<std::string> names;
        for (auto const& [name, value] : parsed.head.headers) { names.push_back(name); }
        CheckEq(names, std::vector<std::string>{ "Host", "X-A", "X-B", "x-a", "X-Raw" }, "wire field order");
    });
}

/**
 * @brief Verify malformed request lines and fields fail consistently at every boundary.
 */
auto rejected_request_syntax() -> void {
    constexpr std::pair<std::string_view, mhttp::Status> CASES[]{
        { "GET  / HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "GET\t/ HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1 \r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "GET /a b HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "GET /#f HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "GET /\x7F HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "GET * HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "G@T / HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "BREW / HTTP/1.1\r\nHost: x\r\n\r\n", mhttp::Status::NotImplemented },
        { "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.2\r\nHost: x\r\n\r\n", mhttp::Status::HttpVersionNotSupported },
        { "GET / HTTP/1.1\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\nHost: x\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: evil@host\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\n X: folded\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\nX : value\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\nX: a\0b\r\n\r\n"sv, mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\nX: a\vb\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\nX: a\x1B\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\nX: a\rb\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\r\nHost: x\r\nX: a\nb\r\n\r\n", mhttp::Status::BadRequest },
        { "GET / HTTP/1.1\nHost: x\n\n", mhttp::Status::BadRequest },
    };
    for (auto const& [wire, status] : CASES) {
        check_all_splits<RequestParser>(wire, [wire, status](auto const& parsed) {
            if (Check(parsed.failure.has_value(), std::format("reject request {:?}", wire))) {
                CheckEq(parsed.failure->status, status, std::format("failure status for {:?}", wire));
            }
            Check(!parsed.complete, "malformed request never completes");
        });
    }
}

/**
 * @brief Verify exact URI, field count, line length, and total-header boundaries.
 */
auto request_limits() -> void {
    constexpr auto WIRE = "GET /1234567 HTTP/1.1\r\nHost: x\r\n\r\n"sv;
    ParserLimits limits;
    limits.uri_bytes = 8;
    check_all_splits<RequestParser>(WIRE, [](auto const& parsed) {
        Check(parsed.complete, "URI exactly at limit is accepted");
    }, limits);
    --limits.uri_bytes;
    check_all_splits<RequestParser>(WIRE, [](auto const& parsed) {
        Check(parsed.failure && parsed.failure->status == mhttp::Status::UriTooLong, "URI limit plus one returns 414");
    }, limits);
    RequestParser early{ limits };
    std::string const partial{ "GET /123456789012345" };
    auto const rejected = early.Feed(partial);
    Check(rejected.event == ParseEvent::Failed && rejected.consumed < partial.size(),
        "overlong URI fails before a line terminator arrives");
    constexpr auto SMALL = "GET / HTTP/1.1\r\nHost: x\r\n\r\n"sv;
    limits = {};
    limits.header_bytes = "Host: x\r\n\r\n"sv.size();
    limits.header_line_bytes = "Host: x"sv.size();
    limits.header_fields = 1;
    check_all_splits<RequestParser>(SMALL, [](auto const& parsed) { Check(parsed.complete, "all header limits exactly met"); }, limits);
    for (unsigned which{}; which < 3; ++which) {
        auto restricted = limits;
        if (which == 0) { --restricted.header_bytes; }
        if (which == 1) { --restricted.header_line_bytes; }
        if (which == 2) { --restricted.header_fields; }
        check_all_splits<RequestParser>(SMALL, [](auto const& parsed) {
            Check(parsed.failure && parsed.failure->status == mhttp::Status::RequestHeaderFieldsTooLarge,
                "header bound plus one returns 431");
        }, restricted);
    }
}

/**
 * @brief Verify length framing, absent framing, duplicates, and terminal smuggling rejection.
 */
auto content_length_and_smuggling() -> void {
    for (auto method : { "POST", "PUT", "PATCH", "DELETE" }) {
        auto const wire = std::string{ method } + " / HTTP/1.1\r\nHost: x\r\n\r\n";
        check_all_splits<RequestParser>(wire, [](auto const& parsed) {
            Check(parsed.complete && parsed.body.empty(), "unframed method has empty body without waiting for EOF");
        });
    }
    constexpr auto BODY = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nContent-Length: 03\r\n\r\na\0b"sv;
    ParserLimits limits;
    limits.body_bytes = 3;
    check_all_splits<RequestParser>(BODY, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "equal decimal lengths at body limit are accepted");
        CheckEq(parsed.body, "a\0b"sv, "fixed body preserves NUL");
    }, limits);
    --limits.body_bytes;
    check_all_splits<RequestParser>(BODY, [](auto const& parsed) {
        Check(parsed.failure && parsed.failure->status == mhttp::Status::ContentTooLarge, "declared body over limit returns 413");
        CheckEq(parsed.header_events, std::size_t{}, "oversized body is rejected before headers-complete event");
    }, limits);
    for (auto value : { "x", "+3", "0x3", "3, 3", "", "18446744073709551616" }) {
        auto const wire = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::string{ value } + "\r\n\r\nabc";
        check_all_splits<RequestParser>(wire, [value](auto const& parsed) {
            Check(parsed.failure && parsed.failure->status == mhttp::Status::BadRequest,
                std::format("reject Content-Length {:?}", value));
        });
    }
    for (auto fields : { "Content-Length: 3\r\nContent-Length: 4\r\n", "Content-Length: 3\r\nTransfer-Encoding: chunked\r\n",
                         "Transfer-Encoding: chunked, gzip\r\n", "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n" }) {
        std::string const wire = "POST / HTTP/1.1\r\nHost: x\r\n" + std::string{ fields }
            + "\r\nGET /smuggled HTTP/1.1\r\nHost: x\r\n\r\n";
        RequestParser parser;
        mhttp::test::ParsedMessage<RequestParser> parsed;
        mhttp::test::drain_parser(parser, wire, parsed);
        Check(parsed.failure && parsed.failure->status == mhttp::Status::BadRequest, "ambiguous framing is fatal");
        Check(parsed.consumed < wire.size(), "failure leaves the smuggled successor unconsumed");
        auto const repeated = parser.Feed(std::span{ wire.data(), wire.size() }.subspan(parsed.consumed));
        Check(repeated.event == ParseEvent::Failed && repeated.consumed == 0, "failed parser cannot consume a successor");
    }
}

/**
 * @brief Verify decoded chunks, ignored extensions, and separate bounded trailers.
 */
auto chunked_requests() -> void {
    constexpr auto WIRE = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: ChUnKeD\r\n\r\n"
        "2;key=value\r\nab\r\n1\r\n\0\r\n0\r\nX-Sum: yes\r\nHost: evil\r\nContent-Length: 99\r\n\r\n"sv;
    ParserLimits limits;
    limits.body_bytes = 3;
    check_all_splits<RequestParser>(WIRE, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "chunked body completes at exact limit");
        CheckEq(parsed.body, "ab\0"sv, "chunks concatenate without framing");
        CheckEq(parsed.trailers.Get("X-Sum"), "yes"sv, "trailer is exposed separately");
        Check(!parsed.head.headers.Has("X-Sum"), "trailer never enters initial headers");
        Check(!parsed.trailers.Has("Host") && !parsed.trailers.Has("Content-Length"), "forbidden trailers are discarded");
        CheckEq(parsed.head.headers.Get("Host"), "x"sv, "trailer cannot overwrite routing authority");
    }, limits);
    --limits.body_bytes;
    check_all_splits<RequestParser>(WIRE, [](auto const& parsed) {
        Check(parsed.failure && parsed.failure->status == mhttp::Status::ContentTooLarge, "decoded chunks share the body limit");
    }, limits);
    for (auto chunk : { "nothex\r\n", "NaN\r\n", "-1\r\n", "+1\r\n", "10000000000000000\r\n",
                        "1;ext=a\nb\r\n", "1;ext=a\rb\r\n", "1;ext=a\x01\r\n", "1\n" }) {
        auto const wire = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n" + std::string{ chunk };
        check_all_splits<RequestParser>(wire, [chunk](auto const& parsed) {
            Check(parsed.failure && parsed.failure->status == mhttp::Status::BadRequest,
                std::format("reject malformed chunk {:?}", chunk));
        });
    }
    limits = {};
    limits.header_fields = 2;
    check_all_splits<RequestParser>(WIRE, [](auto const& parsed) {
        Check(parsed.failure && parsed.failure->status == mhttp::Status::RequestHeaderFieldsTooLarge,
            "trailers count toward the same field bound");
    }, limits);
}

/**
 * @brief Verify chunk line, cumulative trailer, and empty-body limit boundaries.
 */
auto chunk_and_trailer_limits() -> void {
    constexpr auto PREFIX = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"sv;
    std::string const wire = std::string{ PREFIX } + "1;ab=x\r\na\r\n0\r\nX-Sum: yes\r\nHost: ignored\r\n\r\n";
    ParserLimits limits;
    limits.header_bytes = "Host: x\r\nTransfer-Encoding: chunked\r\n\r\n"sv.size()
        + "X-Sum: yes\r\nHost: ignored\r\n\r\n"sv.size();
    limits.header_fields = 4;
    limits.chunk_line_bytes = "1;ab=x"sv.size();
    limits.body_bytes = 1;
    check_all_splits<RequestParser>(wire, [](auto const& parsed) {
        Check(parsed.complete && parsed.body == "a", "chunk and trailer bounds exactly met");
    }, limits);
    for (unsigned which{}; which < 3; ++which) {
        auto restricted = limits;
        if (which == 0) { --restricted.header_bytes; }
        if (which == 1) { --restricted.header_fields; }
        if (which == 2) { --restricted.chunk_line_bytes; }
        check_all_splits<RequestParser>(wire, [which](auto const& parsed) {
            Check(parsed.failure && parsed.failure->status == (which == 2 ? mhttp::Status::BadRequest
                : mhttp::Status::RequestHeaderFieldsTooLarge), "chunk or trailer bound plus one fails");
        }, restricted);
    }
    limits = {};
    limits.body_bytes = 0;
    check_all_splits<RequestParser>(std::string{ PREFIX } + "0\r\n\r\n", [](auto const& parsed) {
        Check(parsed.complete && parsed.body.empty(), "zero chunk completes at a zero-byte body limit");
    }, limits);
    for (auto chunks : { "1\r\nax\n0\r\n\r\n", "1\r\na\rX0\r\n\r\n", "1\r\na0\r\n\r\n" }) {
        check_all_splits<RequestParser>(std::string{ PREFIX } + chunks, [](auto const& parsed) {
            Check(parsed.failure && parsed.failure->status == mhttp::Status::BadRequest,
                "chunk data requires its exact CRLF terminator");
        });
    }
    std::string binary;
    for (unsigned index{}; index < 16 * 1024; ++index) { binary.push_back(static_cast<char>(index % 256)); }
    std::string const large_wire = std::string{ PREFIX } + "4000\r\n" + binary + "\r\n0\r\n\r\n";
    check_all_splits<RequestParser>(large_wire, [&binary](auto const& parsed) {
        Check(parsed.complete && parsed.body == binary, "large binary chunk survives every split");
    });
}

/**
 * @brief Verify transfer, Connection, and Expect handling through actual parser header events.
 */
auto request_connection_semantics() -> void {
    constexpr std::pair<std::string_view, mhttp::Status> INVALID[]{
        { "Transfer-Encoding: gzip, chunked\r\n", mhttp::Status::NotImplemented },
        { "Transfer-Encoding: gzip\r\n", mhttp::Status::NotImplemented },
        { "Transfer-Encoding: chunked,\r\n", mhttp::Status::BadRequest },
        { "Transfer-Encoding: chunked;param=x\r\n", mhttp::Status::BadRequest },
        { "Expect: unsupported\r\n", mhttp::Status::ExpectationFailed },
    };
    for (auto const& [fields, status] : INVALID) {
        auto const wire = "POST / HTTP/1.1\r\nHost: x\r\n" + std::string{ fields } + "\r\n";
        check_all_splits<RequestParser>(wire, [status](auto const& parsed) {
            Check(parsed.failure && parsed.failure->status == status, "parser rejects unsupported framing or expectation");
        });
    }
    check_all_splits<RequestParser>("POST / HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", [](auto const& parsed) {
        Check(parsed.failure && parsed.failure->status == mhttp::Status::BadRequest, "HTTP/1.0 cannot carry transfer coding");
    });
    check_all_splits<RequestParser>("POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-CoNtInUe\r\n"
        "Connection: notclose, keep-alive\r\nConnection: CLOSE\r\n\r\n", [](auto const& parsed) {
        Check(parsed.complete && parsed.framing.expect_continue && !parsed.framing.keep_alive,
            "request events expose expectation and multiline close semantics");
    });
    check_all_splits<RequestParser>("POST / HTTP/1.0\r\nExpect: unsupported\r\nConnection: KEEP-ALIVE\r\n\r\n", [](auto const& parsed) {
        Check(parsed.complete && !parsed.framing.expect_continue && parsed.framing.keep_alive,
            "HTTP/1.0 ignores expectations and honors case-insensitive keep-alive");
    });
    std::string const pathological = "GET / HTTP/1.1\r\nHost: x\r\nX" + std::string(32 * 1024, ' ') + ": y\r\n\r\n";
    std::array const cuts{ pathological.size() };
    auto const rejected = mhttp::test::parse_wire<RequestParser>(pathological, cuts);
    Check(rejected.failure.has_value(), "pathological field-name whitespace is rejected within the case deadline");
}

/**
 * @brief Verify successor bytes remain caller-owned and empty Feed emits final completion.
 */
auto pipelining_and_incomplete_input() -> void {
    constexpr auto FIRST = "POST /one HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc"sv;
    constexpr auto SECOND = "GET /two HTTP/1.1\r\nHost: x\r\n\r\n"sv;
    std::string const wire = std::string{ FIRST } + std::string{ SECOND };
    RequestParser parser;
    mhttp::test::ParsedMessage<RequestParser> first;
    mhttp::test::drain_parser(parser, wire, first);
    Check(first.complete && first.consumed == FIRST.size(), "first message leaves pipelined bytes unconsumed");
    parser.Reset();
    mhttp::test::ParsedMessage<RequestParser> second;
    mhttp::test::drain_parser(parser, std::span{ wire.data(), wire.size() }.subspan(first.consumed), second);
    Check(second.complete && second.head.target == "/two", "reset parses the retained successor");
    for (auto partial : { "GET / HTTP/1.1\r", "GET / HTTP/1.1\r\nHost: x\r\n",
                          "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\na",
                          "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n2\r\na" }) {
        std::string_view const text{ partial };
        std::array const cuts{ text.size() };
        auto const parsed = mhttp::test::parse_wire<RequestParser>(text, cuts);
        Check(parsed.failure && parsed.failure->error == mhttp::Error::ConnectionClosed,
            "EOF during incomplete framing reports truncation");
    }
}

/**
 * @brief Run request parser cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "request lines and headers", request_lines_and_headers },
        { "rejected request syntax", rejected_request_syntax },
        { "request limits", request_limits },
        { "length framing and smuggling", content_length_and_smuggling },
        { "chunked requests", chunked_requests },
        { "chunk and trailer limits", chunk_and_trailer_limits },
        { "request connection semantics", request_connection_semantics },
        { "pipelining and incomplete input", pipelining_and_incomplete_input },
    });
}
