/**
 * @file route_pattern_test.cpp
 * @brief Verify route grammar, regular-expression admission, and path-segment decoding.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Recognize exact, parameter, regex, and terminal wildcard edges without losing empty segments.
 */
auto route_grammar() -> void {
    auto parsed = mhttp::detail::parse_route_pattern("/users/:id/{kind:[a-z]+}/*");
    if (!Check(parsed.has_value(), "mixed route parses")) { return; }
    using enum mhttp::detail::RouteSegmentKind;
    CheckEq(parsed->size(), std::size_t{ 5 }, "leading empty segment is retained");
    CheckEq((*parsed)[0].kind, Static, "leading slash is an exact empty edge");
    CheckEq((*parsed)[1].key, "users"sv, "exact bytes preserved");
    CheckEq((*parsed)[2].kind, Parameter, "named parameter");
    CheckEq((*parsed)[2].name, "id"sv, "parameter name excludes colon");
    CheckEq((*parsed)[3].kind, Regex, "regular expression parameter");
    CheckEq((*parsed)[3].name, "kind"sv, "regex name excludes braces");
    Check(std::regex_match("abc", *(*parsed)[3].pattern), "expression was compiled at registration");
    CheckEq((*parsed)[4].kind, Wildcard, "last edge consumes the remainder");
    CheckEq((*parsed)[4].name, "*"sv, "wildcard capture name");
    auto trailing = mhttp::detail::parse_route_pattern("/prefix:suffix//");
    Check(trailing && trailing->size() == 4 && trailing->back().key.empty(), "empty and trailing segments survive");
    Check(trailing && (*trailing)[1].kind == Static, "colon inside a segment is literal");
    auto asterisk = mhttp::detail::parse_route_pattern("*");
    Check(asterisk && asterisk->front().kind == Static, "OPTIONS asterisk target is literal");
    auto empty_regex = mhttp::detail::parse_route_pattern("/{empty:}");
    Check(empty_regex && std::regex_match("", *empty_regex->back().pattern), "empty expression can match an empty segment");
    for (auto pattern : { "", "relative", "/x?query", "/x#fragment", "/:", "/:bad name", "/:*", "/{x}",
             "/{:x}", "/{x:[}", "/{x:abc", "/x/*/more", "/:id/:id", "/:id/{id:a}" }) {
        auto rejected = mhttp::detail::parse_route_pattern(pattern);
        Check(!rejected && rejected.error() == mhttp::Error::InvalidRequest,
            std::format("reject invalid pattern {:?}", pattern));
    }
}

/**
 * @brief Reject quantified alternation and nested quantifiers through intermediate grouping.
 */
auto regex_admission() -> void {
    for (auto pattern : { "(a+)+", "(a*)*", "(a{2,})+", "(a|aa)+", "((a+))+", "((a|aa))+", "(a?)+",
             "(?:a+)+", "((?:a)+)+", "(a+)?", "(a|b){2}" }) {
        Check(mhttp::detail::has_redos_shape(pattern), std::format("detect risky shape {:?}", pattern));
        auto route = std::format("/{{id:{}}}", pattern);
        Check(!mhttp::detail::parse_route_pattern(route), std::format("reject risky route {:?}", route));
    }
    for (auto pattern : { "[0-9]+", "[a-zA-Z0-9_-]+", "(foo|bar)", "(?:ab)+", "(ab)+", "((ab))+",
             R"([()+*?|{}]+)", R"(\(a\+\)\+)", "[a-z]{2,8}", "", "(?=ab)ab", "(ab)?" }) {
        Check(!mhttp::detail::has_redos_shape(pattern), std::format("admit safe shape {:?}", pattern));
        auto route = std::format("/{{id:{}}}", pattern);
        Check(mhttp::detail::parse_route_pattern(route).has_value(), std::format("compile admitted route {:?}", route));
    }
}

/**
 * @brief Check exact and exceeded source limits and invalid regex compilation before registration.
 */
auto regex_limits() -> void {
    auto exact = mhttp::detail::parse_route_pattern("/{x:" + std::string(256, 'a') + "}");
    Check(exact.has_value(), "expression exactly at default source limit");
    auto over = mhttp::detail::parse_route_pattern("/{x:" + std::string(257, 'a') + "}");
    Check(!over && over.error() == mhttp::Error::InvalidRequest, "expression over source limit");
    mhttp::detail::RouteLimits limits{ .regex_pattern_bytes = 3 };
    Check(mhttp::detail::parse_route_pattern("/{x:abc}", limits).has_value(), "configured source limit accepted");
    Check(!mhttp::detail::parse_route_pattern("/{x:abcd}", limits), "configured source limit exceeded");
    for (auto pattern : { "/{x:(}", "/{x:a{4,2}}", "/{x:*}", "/{x:[abc}" }) {
        Check(!mhttp::detail::parse_route_pattern(pattern), std::format("compile rejects invalid regex {:?}", pattern));
    }
}

/**
 * @brief Decode after splitting, preserving binary bytes, malformed escapes, plus, and empty segments.
 */
auto path_decoding() -> void {
    auto segments = mhttp::detail::decode_path_segments("/a%2Fb/hello%20world/a+b/%00/%ZZ/%/");
    CheckEq(segments, std::vector<std::string>{ "", "a/b", "hello world", "a+b", std::string(1, '\0'), "%ZZ", "%", "" },
        "encoded separators remain inside the original segment");
    CheckEq(mhttp::detail::decode_path_segments("//"), std::vector<std::string>{ "", "", "" }, "consecutive slashes");
    CheckEq(mhttp::detail::decode_path_segments("/"), std::vector<std::string>{ "", "" }, "root includes final empty segment");
}

/**
 * @brief Run bounded route-pattern cases with optional name filters.
 * @param argc Argument count.
 * @param argv Borrowed command-line arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "route grammar", route_grammar },
        { "regular expression admission", regex_admission },
        { "regular expression source limits", regex_limits },
        { "path segment decoding", path_decoding },
    });
}
