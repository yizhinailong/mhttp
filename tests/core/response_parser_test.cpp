/**
 * @file response_parser_test.cpp
 * @brief Verify strict response lines, body semantics, EOF framing, and bounded interim responses.
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
 * @brief Verify optional reasons, custom reasons, and fixed-length binary body delivery.
 */
auto response_lines() -> void {
    constexpr std::pair<std::string_view, std::string_view> CASES[]{
        { "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na\0b"sv, "OK" },
        { "HTTP/1.1 200\r\nContent-Length: 3\r\n\r\na\0b"sv, "" },
        { "HTTP/1.1 200 \r\nContent-Length: 3\r\n\r\na\0b"sv, "" },
        { "HTTP/1.0 200 A custom\treason\r\nContent-Length: 3\r\n\r\na\0b"sv, "A custom\treason" },
    };
    for (auto const& [wire, reason] : CASES) {
        check_all_splits<ResponseParser>(wire, [wire, reason](auto const& parsed) {
            Check(parsed.complete && !parsed.failure, std::format("response completes {:?}", wire));
            CheckEq(parsed.head.status, mhttp::Status::Ok, "numeric status parsed");
            CheckEq(parsed.head.reason, reason, "reason preserves exact bytes or absence");
            CheckEq(parsed.body, "a\0b"sv, "response body preserves NUL");
            CheckEq(parsed.consumed, wire.size(), "one response consumed");
            CheckEq(parsed.header_events, std::size_t{ 1 }, "one final-header event");
        });
    }
    for (auto line : { "HTTP/2 200 OK", "HTTP/1.2 200 OK", "HTTP/1.1 20 OK", "HTTP/1.1 2000 OK",
                       "HTTP/1.1 200OK", "HTTP/1.1  200 OK", "HTTP/1.1 000 Unknown", "HTTP/1.1 2a0 OK",
                       "HTTP/1.1 200 a\rb", "HTTP/1.1 200 a\nb", "HTTP/1.1 200 a\vb", "HTTP/1.1 200 a\x7F" }) {
        auto const wire = std::string{ line } + "\r\nContent-Length: 0\r\n\r\n";
        check_all_splits<ResponseParser>(wire, [line](auto const& parsed) {
            Check(parsed.failure && parsed.failure->error == mhttp::Error::ProtocolError,
                std::format("reject malformed status line {:?}", line));
        });
    }
}

/**
 * @brief Verify HEAD, 204, and 304 end after headers despite Content-Length metadata.
 */
auto bodyless_responses() -> void {
    for (auto status : { "204 No Content", "304 Not Modified" }) {
        auto const head = "HTTP/1.1 " + std::string{ status } + "\r\nContent-Length: 18446744073709551615\r\n\r\n";
        auto const wire = head + "left-for-successor";
        check_all_splits<ResponseParser>(wire, [&head](auto const& parsed) {
            Check(parsed.complete && !parsed.failure && parsed.body.empty(), "bodyless status ignores length metadata");
            CheckEq(parsed.consumed, head.size(), "bodyless response leaves following bytes untouched");
        });
    }
    constexpr auto HEAD = "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551615\r\n\r\n"sv;
    std::string const wire = std::string{ HEAD } + "not-a-HEAD-body";
    for (std::size_t split{}; split <= wire.size(); ++split) {
        std::array const cuts{ split, wire.size() };
        auto const parsed = mhttp::test::parse_wire<ResponseParser>(wire, cuts, ResponseParser{ mhttp::Method::Head });
        Check(parsed.complete && !parsed.failure && parsed.body.empty(), "HEAD method forbids a response body");
        CheckEq(parsed.consumed, HEAD.size(), "HEAD leaves following bytes untouched");
    }
}

/**
 * @brief Verify fixed, chunked, and close-delimited body limits and truncated framing.
 */
auto response_body_framing() -> void {
    constexpr auto EOF_BODY = "HTTP/1.0 200 OK\r\nX-Info: yes\r\n\r\na\0b"sv;
    check_all_splits<ResponseParser>(EOF_BODY, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "EOF-delimited body completes on closure");
        CheckEq(parsed.body, "a\0b"sv, "EOF body preserves bytes");
        Check(!parsed.framing.keep_alive, "EOF-delimited response cannot reuse the connection");
    });
    constexpr auto CHUNKED = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n"
        "1\r\na\r\n2;ext=x\r\n\0b\r\n0\r\nX-Sum: yes\r\nContent-Type: evil\r\n\r\n"sv;
    ParserLimits limits;
    limits.body_bytes = 3;
    check_all_splits<ResponseParser>(CHUNKED, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure, "chunked response completes");
        CheckEq(parsed.body, "a\0b"sv, "chunked response decodes bytes");
        CheckEq(parsed.trailers.Get("X-Sum"), "yes"sv, "response trailers remain separate");
        CheckEq(parsed.head.headers.Get("Content-Type"), "text/plain"sv, "forbidden content-processing trailer cannot overwrite headers");
        Check(!parsed.trailers.Has("Content-Type"), "forbidden response trailer is discarded");
    }, limits);
    constexpr auto FIXED = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na\0b"sv;
    for (auto wire : { FIXED, CHUNKED, EOF_BODY }) {
        check_all_splits<ResponseParser>(wire, [](auto const& parsed) {
            Check(parsed.complete && parsed.body.size() == 3, "all framing modes accept an exact body limit");
        }, limits);
        auto smaller = limits;
        --smaller.body_bytes;
        check_all_splits<ResponseParser>(wire, [](auto const& parsed) {
            Check(parsed.failure && parsed.failure->error == mhttp::Error::BodyTooLarge,
                "all framing modes reject the body limit plus one");
        }, smaller);
    }
    for (auto wire : { "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\na"sv,
                       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\na"sv,
                       "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n"sv }) {
        check_all_splits<ResponseParser>(wire, [](auto const& parsed) {
            Check(parsed.failure && parsed.failure->error == mhttp::Error::ConnectionClosed,
                "EOF in fixed or chunked framing reports truncation");
        });
    }
    for (auto fields : { "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n", "Content-Length: 1\r\nContent-Length: 2\r\n",
                         "Transfer-Encoding: gzip\r\n", "Transfer-Encoding: chunked, gzip\r\n" }) {
        auto const wire = "HTTP/1.1 200 OK\r\n" + std::string{ fields } + "\r\n";
        check_all_splits<ResponseParser>(wire, [](auto const& parsed) {
            Check(parsed.failure && parsed.failure->error == mhttp::Error::ProtocolError, "ambiguous or unsupported response framing fails");
        });
    }
}

/**
 * @brief Verify bounded informational-response skipping and absence of their fields in the final head.
 */
auto interim_responses() -> void {
    constexpr auto INTERIM = "HTTP/1.1 103 Early Hints\r\nX-Interim: hidden\r\n\r\n"sv;
    constexpr auto FINAL = "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nX-Final: yes\r\n\r\nx"sv;
    ParserLimits limits;
    limits.interim_responses = 2;
    std::string const wire = "HTTP/1.1 100 Continue\r\n\r\n" + std::string{ INTERIM } + std::string{ FINAL };
    check_all_splits<ResponseParser>(wire, [](auto const& parsed) {
        Check(parsed.complete && !parsed.failure && parsed.body == "x", "bounded informational responses are skipped");
        CheckEq(parsed.header_events, std::size_t{ 1 }, "only the final response emits headers-complete");
        Check(!parsed.head.headers.Has("X-Interim"), "interim fields do not leak into the final response");
        CheckEq(parsed.head.headers.Get("X-Final"), "yes"sv, "final fields remain available");
    }, limits);
    --limits.interim_responses;
    check_all_splits<ResponseParser>(wire, [](auto const& parsed) {
        Check(parsed.failure && parsed.failure->error == mhttp::Error::ProtocolError, "interim limit plus one fails");
        CheckEq(parsed.header_events, std::size_t{}, "over-limit stream never exposes a final response");
    }, limits);
    check_all_splits<ResponseParser>(INTERIM, [](auto const& parsed) {
        Check(parsed.failure && parsed.failure->error == mhttp::Error::ConnectionClosed,
            "EOF after an interim response still requires a final response");
    });
}

/**
 * @brief Observe bounded 1xx metadata at every split without consuming following response bytes prematurely.
 */
auto observable_interim_responses() -> void {
    constexpr auto FIRST = "HTTP/1.1 103 Early Hints\r\nLink: </asset>\r\n\r\n"sv;
    constexpr auto SECOND = "HTTP/1.1 100 Continue\r\nX-Interim: yes\r\n\r\n"sv;
    constexpr auto FINAL = "HTTP/1.1 201 Created\r\nContent-Length: 3\r\n\r\nabc"sv;
    auto const wire = std::string{ FIRST } + std::string{ SECOND } + std::string{ FINAL };
    for (std::size_t split{}; split <= wire.size(); ++split) {
        std::array const cuts{ split, wire.size() };
        auto const parsed = mhttp::test::parse_wire<ResponseParser>(wire, cuts,
            ResponseParser{ mhttp::Method::Post, { .interim_responses = 2 }, true });
        Check(parsed.complete && !parsed.failure && parsed.body == "abc", "observable interims preserve final body at every split");
        if (CheckEq(parsed.informational.size(), std::size_t{ 2 }, "one event per complete 1xx head")) {
            CheckEq(parsed.informational[0].status, mhttp::Status::EarlyHints, "103 remains distinct from permission to upload");
            CheckEq(parsed.informational[0].headers.Get("Link"), "</asset>"sv, "snapshot retains 103 fields");
            CheckEq(parsed.informational[1].status, mhttp::Status::Continue, "100 is observable before final headers");
            CheckEq(parsed.informational[1].headers.Get("X-Interim"), "yes"sv, "second interim owns separate fields");
        }
        CheckEq(parsed.header_events, std::size_t{ 1 }, "only final response emits HeadersComplete");
        CheckEq(parsed.head.status, mhttp::Status::Created, "final head replaces every interim");
        Check(!parsed.head.headers.Has("X-Interim"), "interim fields do not leak");
        CheckEq(parsed.consumed, wire.size(), "all supplied response bytes consumed exactly once");
    }
    ResponseParser parser{ mhttp::Method::Post, { .interim_responses = 1 }, true };
    auto first = parser.Feed(FIRST);
    Check(first.event == ParseEvent::Informational && first.consumed == FIRST.size(), "interim event stops precisely at its terminator");
    auto second = parser.Feed(SECOND);
    Check(second.event == ParseEvent::Failed && second.failure->error == mhttp::Error::ProtocolError,
        "observable interims keep the same admission limit");
    ResponseParser eof{ mhttp::Method::Post, {}, true };
    Check(eof.Feed(SECOND).event == ParseEvent::Informational, "100 is observable without a coalesced final response");
    Check(eof.Finish().event == ParseEvent::Failed, "EOF after an observable 100 still requires a final response");
    ResponseParser reusable{ mhttp::Method::Post, { .interim_responses = 2 }, true };
    for (unsigned repeat{}; repeat < 2; ++repeat) {
        mhttp::test::ParsedMessage<ResponseParser> parsed;
        for (char const& byte : wire) { mhttp::test::drain_parser(reusable, { &byte, 1 }, parsed); }
        Check(parsed.complete && parsed.informational.size() == 2, "bytewise input and Reset preserve opt-in mode with fresh limits");
        reusable.Reset();
    }
}

/**
 * @brief Verify huge declared lengths never reserve body storage before bytes arrive.
 */
auto huge_lengths_and_eof_events() -> void {
    ParserLimits limits;
    limits.body_bytes = std::numeric_limits<std::uint64_t>::max();
    ResponseParser parser{ limits };
    constexpr auto WIRE = "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551615\r\n\r\nx"sv;
    mhttp::test::ParsedMessage<ResponseParser> parsed;
    mhttp::test::drain_parser(parser, WIRE, parsed);
    Check(!parsed.complete && !parsed.failure && parsed.header_events == 1,
        "huge length produces normal events without reserve or allocation failure");
    CheckEq(parsed.body, "x"sv, "only the received body byte is stored by the test consumer");
    auto const truncated = parser.Finish();
    Check(truncated.failure && truncated.failure->error == mhttp::Error::ConnectionClosed,
        "huge declared length still detects premature closure");
    ResponseParser eof_parser;
    parsed = {};
    mhttp::test::drain_parser(eof_parser, "HTTP/1.1 200 OK\r\n\r\nx"sv, parsed);
    Check(!parsed.complete && !parsed.failure, "close-delimited response remains incomplete while open");
    Check(eof_parser.Finish().event == ParseEvent::Complete, "EOF explicitly completes the body");
    Check(eof_parser.Feed({}).event == ParseEvent::Complete, "completed EOF state stays stable");
}

/**
 * @brief Run response parser cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "response lines", response_lines },
        { "bodyless responses", bodyless_responses },
        { "response body framing", response_body_framing },
        { "interim responses", interim_responses },
        { "observable interim responses", observable_interim_responses },
        { "huge lengths and EOF events", huge_lengths_and_eof_events },
    });
}
