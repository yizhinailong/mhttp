/**
 * @file url_test.cpp
 * @brief Verify HTTP URL ownership, authority validation, and RFC relative-reference examples.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Verify owned components, IPv6, default paths, and explicit empty query/fragment markers.
 */
auto absolute_urls() -> void {
    std::string source{ "HTTP://Example.COM:8080/a%2Fb?x=1&x=2#fragment" };
    auto const url = mhttp::Url::Parse(source);
    if (!Check(url.has_value(), "parse absolute URL")) { return; }
    source.clear();
    CheckEq(url->Scheme(), "http"sv, "scheme normalization");
    CheckEq(url->Host(), "example.com"sv, "owned host normalization");
    CheckEq(url->Port(), std::uint16_t{ 8080 }, "explicit port");
    CheckEq(url->Path(), "/a%2Fb"sv, "path remains encoded");
    CheckEq(url->Query(), "x=1&x=2"sv, "query retains order and repetitions");
    CheckEq(url->Fragment(), "fragment"sv, "fragment retained separately");
    CheckEq(url->Target(), "/a%2Fb?x=1&x=2"sv, "fragment never enters request target");
    CheckEq(url->ToString(), "http://example.com:8080/a%2Fb?x=1&x=2#fragment"sv, "recompose normalized URL");
    constexpr std::pair<std::string_view, std::string_view> CASES[]{
        { "http://host", "http://host/" }, { "http://host:80/", "http://host/" },
        { "http://host?x", "http://host/?x" }, { "http://host?", "http://host/?" },
        { "http://host#", "http://host/#" }, { "http://host?#", "http://host/?#" },
        { "http://[::1]:8080/path", "http://[::1]:8080/path" },
        { "http://[2001:DB8::1]:80/", "http://[2001:db8::1]/" },
        { "http://127.0.0.1:65535/", "http://127.0.0.1:65535/" },
        { "http://host:0/", "http://host:0/" }, { "http://host:00080/", "http://host/" },
        { "http://exa%6Dple.com/", "http://example.com/" },
    };
    for (auto const& [input, expected] : CASES) {
        auto const parsed = mhttp::Url::Parse(input);
        if (Check(parsed.has_value(), std::format("parse {:?}", input))) {
            CheckEq(parsed->ToString(), expected, std::format("recompose {:?}", input));
            Check(!parsed->Target().contains('#'), "every target excludes fragments");
        }
    }
    for (auto host : { "::", "::1", "1::", "2001:db8:1:2:3:4:5:6", "::ffff:192.0.2.1", "1:2:3:4:5:6:192.0.2.1" }) {
        auto const input = "http://[" + std::string{ host } + "]/";
        auto const parsed = mhttp::Url::Parse(input);
        Check(parsed.has_value(), std::format("valid IPv6 URL {}", input));
    }
    auto const empty_markers = mhttp::Url::Parse("http://host?#");
    Check(empty_markers->HasQuery() && empty_markers->HasFragment(), "empty component markers remain distinct");
}

/**
 * @brief Reject host confusion, bad brackets, unencoded controls, and overflowing or partial ports.
 */
auto invalid_urls() -> void {
    constexpr auto INPUTS = std::to_array<std::string_view>({
        "", "host/path", "/path", "//host", "1http://host", "http:/host", "http:///path", "http://",
        "http://user:pass@host/", "http://evil.com\\@good.com/", "http://user%40host/",
        "http://host%2Fother/", "http://host%5Cother/", "http://host%00/", "http://host%ZZ/",
        "http://host /", "http://host/has space", "http://host/a\r\nb", "http://host/\x7F",
        "http://[]/", "http://[::1/", "http://[::1]evil.com/", "http://[::1]]/", "http://[x]/",
        "http://[1:2:3:4:5:6:7]/", "http://[1:2:3:4:5:6:7:8:9]/", "http://[1:2:3:4:5:6:7:8::]/",
        "http://[1::2::3]/", "http://[:::]/", "http://[1:]/", "http://[:1]/", "http://[12345::]/",
        "http://[::ffff:192.0.2.999]/", "http://[::ffff:192.0.02.1]/", "http://[::ffff:192.0.2]/",
        "http://[192.0.2.1::]/", "http://::1/", "http://host:/", "http://host:+80/", "http://host:-1/",
        "http://host:65536/", "http://host:12345678901234567890123456/", "http://host:80x/", "http://host:80:90/",
    });
    for (auto input : INPUTS) {
        auto const parsed = mhttp::Url::Parse(input);
        if (Check(!parsed, std::format("reject URL {:?}", input))) {
            CheckEq(parsed.error(), mhttp::Error::InvalidUrl, "invalid syntax returns InvalidUrl");
        }
    }
    Check(!mhttp::Url::Parse("http://host/a\0b"sv), "embedded NUL is rejected without truncation");
    for (auto input : { "https://host/", "HTTPS://host/", "ftp://host/", "custom+v1://host/" }) {
        auto const parsed = mhttp::Url::Parse(input);
        if (Check(!parsed, "unsupported scheme returns failure")) {
            CheckEq(parsed.error(), mhttp::Error::UnsupportedScheme, "schemes are recognized and rejected explicitly");
        }
    }
}

/**
 * @brief Check default-port omission and IPv6 bracket handling in Host generation.
 */
auto host_values() -> void {
    CheckEq(mhttp::MakeHostValue("example.com", 80), "example.com"sv, "omit default HTTP port");
    CheckEq(mhttp::MakeHostValue("example.com", 8080), "example.com:8080"sv, "keep nondefault port");
    CheckEq(mhttp::MakeHostValue("::1", 80), "[::1]"sv, "bare IPv6 gains brackets");
    CheckEq(mhttp::MakeHostValue("[::1]", 80), "[::1]"sv, "existing IPv6 brackets are not doubled");
    CheckEq(mhttp::MakeHostValue("[::1]", 8080), "[::1]:8080"sv, "IPv6 port follows brackets");
    CheckEq(mhttp::MakeHostValue("host", 443, "HTTPS"), "host"sv, "HTTPS default-port recognition");
    CheckEq(mhttp::MakeHostValue("host", 80, "https"), "host:80"sv, "HTTP port is nondefault for HTTPS");
    CheckEq(mhttp::MakeHostValue("host", 0), "host:0"sv, "explicit zero is not a default port");
}

/**
 * @brief Check RFC 3986 section 5.4 HTTP references and abnormal dot-segment examples.
 */
auto relative_references() -> void {
    auto const base = mhttp::Url::Parse("http://a/b/c/d;p?q");
    if (!Check(base.has_value(), "parse RFC example base")) { return; }
    constexpr std::pair<std::string_view, std::string_view> REFERENCES[]{
        { "g", "http://a/b/c/g" }, { "./g", "http://a/b/c/g" }, { "g/", "http://a/b/c/g/" },
        { "/g", "http://a/g" }, { "//g", "http://g/" }, { "?y", "http://a/b/c/d;p?y" },
        { "g?y", "http://a/b/c/g?y" }, { "#s", "http://a/b/c/d;p?q#s" },
        { "g#s", "http://a/b/c/g#s" }, { "g?y#s", "http://a/b/c/g?y#s" },
        { ";x", "http://a/b/c/;x" }, { "g;x", "http://a/b/c/g;x" }, { "g;x?y#s", "http://a/b/c/g;x?y#s" },
        { "", "http://a/b/c/d;p?q" }, { ".", "http://a/b/c/" }, { "./", "http://a/b/c/" },
        { "..", "http://a/b/" }, { "../", "http://a/b/" }, { "../g", "http://a/b/g" },
        { "../..", "http://a/" }, { "../../", "http://a/" }, { "../../g", "http://a/g" },
        { "../../../g", "http://a/g" }, { "../../../../g", "http://a/g" },
        { "/./g", "http://a/g" }, { "/../g", "http://a/g" }, { "g.", "http://a/b/c/g." },
        { ".g", "http://a/b/c/.g" }, { "g..", "http://a/b/c/g.." }, { "..g", "http://a/b/c/..g" },
        { "./../g", "http://a/b/g" }, { "./g/.", "http://a/b/c/g/" },
        { "g/./h", "http://a/b/c/g/h" }, { "g/../h", "http://a/b/c/h" },
        { "g;x=1/./y", "http://a/b/c/g;x=1/y" }, { "g;x=1/../y", "http://a/b/c/y" },
        { "g?y/./x", "http://a/b/c/g?y/./x" }, { "g?y/../x", "http://a/b/c/g?y/../x" },
        { "g#s/./x", "http://a/b/c/g#s/./x" }, { "g#s/../x", "http://a/b/c/g#s/../x" },
        { "?", "http://a/b/c/d;p?" }, { "#", "http://a/b/c/d;p?q#" },
        { "%2e/%2e%2e/g", "http://a/b/c/%2e/%2e%2e/g" },
        { "/a//b/../c/", "http://a/a//c/" }, { "http://b/a/../c", "http://b/c" },
        { "//[::1]:8080/a/../b", "http://[::1]:8080/b" },
    };
    for (auto const& [reference, expected] : REFERENCES) {
        auto const resolved = mhttp::Url::Resolve(*base, reference);
        if (Check(resolved.has_value(), std::format("resolve {:?}", reference))) {
            CheckEq(resolved->ToString(), expected, std::format("resolved URL for {:?}", reference));
        }
    }
    auto const foreign_scheme = mhttp::Url::Resolve(*base, "g:h");
    if (Check(!foreign_scheme, "RFC non-HTTP reference is outside the supported transport")) {
        CheckEq(foreign_scheme.error(), mhttp::Error::UnsupportedScheme, "non-HTTP reference error");
    }
    Check(!mhttp::Url::Resolve(*base, "http:g"), "HTTP reference requires a valid authority");
    for (auto invalid : { "//user@host/", "//[::1]evil/", "//", "has space", "g\r\nInjected:yes" }) {
        Check(!mhttp::Url::Resolve(*base, invalid), std::format("reject unsafe redirect {:?}", invalid));
    }
    auto const fragment_base = mhttp::Url::Parse("http://host?query#old");
    CheckEq(mhttp::Url::Resolve(*fragment_base, "")->ToString(), "http://host/?query"sv,
        "empty reference does not inherit the base fragment");
    CheckEq(mhttp::Url::Resolve(*fragment_base, "new")->ToString(), "http://host/new"sv,
        "relative path merges with an authority-only base");
}

/**
 * @brief Run URL cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "absolute URLs", absolute_urls },
        { "invalid URLs", invalid_urls },
        { "Host values", host_values },
        { "relative URL references", relative_references },
    });
}
