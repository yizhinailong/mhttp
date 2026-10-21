/**
 * @file framing_test.cpp
 * @brief Verify HTTP connection tokens and strict framing before incremental parsing.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace mhttp::detail;

/**
 * @brief Verify whole-token, multiline, and version-dependent persistence rules.
 */
auto connection_tokens() -> void {
    mhttp::Headers headers;
    Check(headers.Add("Connection", "notclose, Upgrade").has_value(), "add connection options");
    Check(allows_keep_alive(mhttp::Version::Http11, headers), "substring does not close HTTP/1.1");
    Check(!allows_keep_alive(mhttp::Version::Http10, headers), "HTTP/1.0 closes by default");
    Check(headers.Add("cONNECTION", "KEEP-ALIVE").has_value(), "add later keep-alive option");
    Check(allows_keep_alive(mhttp::Version::Http10, headers), "HTTP/1.0 honors folded keep-alive token");
    Check(headers.Add("Connection", "custom, cLoSe").has_value(), "add close on later line");
    Check(!allows_keep_alive(mhttp::Version::Http11, headers), "close overrides all other options");
    Check(is_forbidden_trailer("CUSTOM", headers), "connection-nominated trailer is forbidden");
    Check(is_forbidden_trailer("Content-Length", headers), "framing trailer is forbidden");
    Check(!is_forbidden_trailer("X-Checksum", headers), "ordinary trailer is independent metadata");
}

/**
 * @brief Verify strict duplicate lengths, unsupported transfers, and bodyless response precedence.
 */
auto framing_decisions() -> void {
    mhttp::Headers headers;
    Check(headers.Add("Host", "[::1]:8080").has_value(), "prepare valid authority");
    auto framing = analyze_framing(mhttp::Version::Http11, headers, {});
    Check(framing && framing->kind == BodyKind::None, "unframed request completes without EOF");
    Check(headers.Add("Content-Length", "4").has_value(), "add length");
    Check(headers.Add("content-length", "04").has_value(), "add same decimal length");
    framing = analyze_framing(mhttp::Version::Http11, headers, {});
    Check(framing && framing->content_length == 4, "equal numeric lengths are accepted");
    Check(headers.Add("Content-Length", "5").has_value(), "add conflicting length");
    Check(!analyze_framing(mhttp::Version::Http11, headers, {}), "different lengths are rejected");
    headers.Erase("Content-Length");
    Check(headers.Add("Transfer-Encoding", "gzip, chunked").has_value(), "add unknown transfer coding");
    framing = analyze_framing(mhttp::Version::Http11, headers, {});
    Check(!framing && framing.error().status == mhttp::Status::NotImplemented, "unknown coding returns 501");
    Check(headers.Set("Transfer-Encoding", "chunked, gzip").has_value(), "add non-final chunked");
    framing = analyze_framing(mhttp::Version::Http11, headers, {});
    Check(!framing && framing.error().status == mhttp::Status::BadRequest, "non-final chunked returns 400");
    headers.Erase("Transfer-Encoding");
    Check(headers.Add("Content-Length", "18446744073709551615").has_value(), "add huge metadata length");
    framing = analyze_framing(mhttp::Version::Http11, headers, {}, true, true);
    Check(framing && framing->kind == BodyKind::None, "bodyless response ignores body limit for metadata length");
    framing = analyze_framing(mhttp::Version::Http11, headers, {}, true);
    Check(!framing && framing.error().error == mhttp::Error::BodyTooLarge, "body-bearing response applies limit");
}

/**
 * @brief Verify absolute Host authority validation and HTTP/1.1-only expectations.
 */
auto authority_and_expectation() -> void {
    for (auto host : { "host", "host:8080", "[::1]", "[::1]:8080" }) {
        Check(is_valid_host(host), std::format("valid Host {}", host));
    }
    for (auto host : { "", "a@b", "host/path", "host?x", "host#x", "a b", "[::1]extra", "[]" }) {
        Check(!is_valid_host(host), std::format("invalid Host {:?}", host));
    }
    mhttp::Headers headers;
    Check(headers.Add("Host", "host").has_value(), "add Host");
    Check(headers.Add("Expect", "100-CoNtInUe").has_value(), "add expectation");
    auto framing = analyze_framing(mhttp::Version::Http11, headers, {});
    Check(framing && framing->expect_continue, "HTTP/1.1 recognizes expectation");
    framing = analyze_framing(mhttp::Version::Http10, headers, {});
    Check(framing && !framing->expect_continue, "HTTP/1.0 ignores expectation");
    Check(headers.Set("Expect", "other").has_value(), "set unsupported expectation");
    framing = analyze_framing(mhttp::Version::Http11, headers, {});
    Check(!framing && framing.error().status == mhttp::Status::ExpectationFailed, "unsupported expectation returns 417");
}

/**
 * @brief Run framing cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "connection tokens", connection_tokens },
        { "framing decisions", framing_decisions },
        { "authority and expectation", authority_and_expectation },
    });
}
