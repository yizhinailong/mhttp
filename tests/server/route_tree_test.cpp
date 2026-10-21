/**
 * @file route_tree_test.cpp
 * @brief Verify segment routing, backtracking, method dispatch, decoding, and regex subject bounds.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;
using enum mhttp::Method;

/**
 * @brief Verify static priority, empty captures, repeated separators, and exact path extent.
 */
auto segment_matching() -> void {
    mhttp::detail::RouteTree tree;
    Check(tree.Insert(Get, "/users/:id", 1).has_value(), "register parameter first");
    Check(tree.Insert(Get, "/users/new", 2).has_value(), "register static later");
    Check(tree.Insert(Get, "/users/new/", 3).has_value(), "register distinct trailing slash");
    Check(tree.Insert(Get, "/teams/:team/users/:user", 4).has_value(), "multiple captures");
    Check(tree.Insert(Get, "/prefix:suffix", 5).has_value(), "literal middle colon");
    Check(tree.Insert(Get, "/", 6).has_value(), "root route");
    Check(tree.Insert(Get, "/:first/:second/end", 7).has_value(), "consecutive parameters");
    CheckEq(tree.Match(Get, "/users/new").handler, std::optional<std::size_t>{ 2 }, "static wins over parameter");
    Check(tree.Match(Get, "/users/new").params.Empty(), "static route has no stale parameter");
    CheckEq(tree.Match(Get, "/users/new/").handler, std::optional<std::size_t>{ 3 }, "trailing slash is significant");
    auto user = tree.Match(Get, "/users/42");
    CheckEq(user.handler, std::optional<std::size_t>{ 1 }, "parameter selection");
    CheckEq(user.params.Get<int>("id"), std::optional{ 42 }, "strict typed path capture");
    auto empty = tree.Match(Get, "/users/");
    Check(empty.handler == 1 && empty.params.Has("id") && empty.params.Get("id").empty(), "parameter matches empty final segment");
    auto nested = tree.Match(Get, "/teams/red/users/alice");
    CheckEq(nested.params.Get("team"), "red"sv, "first named capture");
    CheckEq(nested.params.Get("user"), "alice"sv, "second named capture");
    auto middle = tree.Match(Get, "///end");
    Check(middle.handler == 7 && middle.params.Count("first") == 1 && middle.params.Get("second").empty(), "empty middle captures");
    CheckEq(tree.Match(Get, "/prefix:suffix").handler, std::optional<std::size_t>{ 5 }, "middle colon remains literal");
    CheckEq(tree.Match(Get, "/").handler, std::optional<std::size_t>{ 6 }, "root match");
    for (auto missing : { "/users", "/users/1/extra", "/teams/a/users", "/missing", "", "users/1" }) {
        auto match = tree.Match(Get, missing);
        Check(!match.handler && match.status == mhttp::Status::NotFound && match.params.Empty(),
            std::format("unmatched path {:?}", missing));
    }
}

/**
 * @brief Backtrack across all edge categories and discard failed-branch captures.
 */
auto branch_backtracking() -> void {
    mhttp::detail::RouteTree tree;
    Check(tree.Insert(Get, "/a/fixed/dead", 1).has_value(), "static dead end");
    Check(tree.Insert(Get, "/a/{digits:[0-9]+}/dead", 2).has_value(), "regex dead end");
    Check(tree.Insert(Get, "/a/{letters:[a-z]+}/dead", 7).has_value(), "matching regex with a dead suffix");
    Check(tree.Insert(Get, "/a/:first/dead", 3).has_value(), "first parameter dead end");
    Check(tree.Insert(Get, "/a/:second/live", 4).has_value(), "second parameter alternative");
    Check(tree.Insert(Get, "/a/*", 5).has_value(), "wildcard fallback");
    Check(tree.Insert(Get, "/a/{digits:[0-9]+}/live", 6).has_value(), "regex live branch");
    auto static_miss = tree.Match(Get, "/a/fixed/live");
    Check(static_miss.handler == 4 && static_miss.params.Get("second") == "fixed", "static path miss falls back to parameter");
    Check(!static_miss.params.Has("first") && !static_miss.params.Has("digits") && !static_miss.params.Has("letters"),
        "failed captures are removed");
    auto regex = tree.Match(Get, "/a/42/live");
    Check(regex.handler == 6 && regex.params.Get("digits") == "42", "regex wins over generic parameter");
    auto wildcard = tree.Match(Get, "/a///remaining/");
    Check(wildcard.handler == 5 && wildcard.params.Get("*") == "//remaining/", "wildcard preserves empty remainder segments");
    CheckEq(wildcard.params.Size(), std::size_t{ 1 }, "wildcard has no failed-branch captures");
    auto empty = tree.Match(Get, "/a/");
    Check(empty.handler == 5 && empty.params.Has("*") && empty.params.Get("*").empty(), "wildcard matches one empty segment");
    Check(!tree.Match(Get, "/a").handler, "wildcard requires its preceding slash");
    mhttp::detail::RouteTree ordered;
    Check(ordered.Insert(Get, "/{first:[a-z]+}", 10).has_value(), "first overlapping regex");
    Check(ordered.Insert(Get, "/{second:.*}", 11).has_value(), "second overlapping regex");
    CheckEq(ordered.Match(Get, "/abc").handler, std::optional<std::size_t>{ 10 }, "registration order breaks regex ties");
    Check(ordered.Insert(Get, "/{first:[a-z]+}", 12).has_value(), "replace identical registration");
    CheckEq(ordered.Match(Get, "/abc").handler, std::optional<std::size_t>{ 12 }, "replacement keeps original priority");
}

/**
 * @brief Distinguish 404/405, union all eligible methods, and prefer explicit HEAD before GET fallback.
 */
auto method_dispatch() -> void {
    mhttp::detail::RouteTree tree;
    Check(tree.Insert(Get, "/resource", 1).has_value(), "GET route");
    Check(tree.Insert(Post, "/resource", 2).has_value(), "POST route");
    Check(tree.Insert(Put, "/:item", 3).has_value(), "PUT generic route");
    CheckEq(tree.Match(Put, "/resource").handler, std::optional<std::size_t>{ 3 }, "method miss backtracks from static to parameter");
    auto mismatch = tree.Match(Delete, "/resource");
    Check(mismatch.status == mhttp::Status::MethodNotAllowed && !mismatch.handler, "matched path with missing method is 405");
    CheckEq(mismatch.allow, "GET, HEAD, POST, PUT"sv, "Allow unions eligible branches and implicit HEAD");
    Check(mismatch.params.Empty(), "405 has no failed candidate captures");
    auto head = tree.Match(Head, "/resource");
    Check(head.handler == 1 && head.head_fallback, "HEAD falls back to GET");
    Check(tree.Insert(Head, "/:item", 4).has_value(), "explicit HEAD route");
    auto explicit_head = tree.Match(Head, "/resource");
    Check(explicit_head.handler == 4 && !explicit_head.head_fallback, "explicit HEAD takes precedence over GET fallback");
    CheckEq(tree.Match("CUSTOM", "/resource").status, mhttp::Status::MethodNotAllowed, "unknown valid method still gets 405");
    CheckEq(tree.Match("get", "/resource").status, mhttp::Status::MethodNotAllowed, "method lookup is case sensitive");
    auto absent = tree.Match(Delete, "/missing/path");
    Check(absent.status == mhttp::Status::NotFound && absent.allow.empty(), "path miss is 404 with no Allow");
    Check(tree.Insert(Options, "*", 5).has_value(), "asterisk target route");
    CheckEq(tree.Match(Options, "*").handler, std::optional<std::size_t>{ 5 }, "literal OPTIONS asterisk target");
    Check(!tree.Insert(static_cast<mhttp::Method>(-1), "/resource", 99), "invalid enum registration rejected");
    Check(!tree.Insert(Get, "/resource/{bad:(a+)+}", 99), "invalid pattern registration rejected");
    CheckEq(tree.Match(Get, "/resource").handler, std::optional<std::size_t>{ 1 }, "failed insertion preserves existing handler");
}

/**
 * @brief Decode paths after splitting and preserve owned binary captures through subsequent matches.
 */
auto encoded_matching() -> void {
    mhttp::detail::RouteTree tree;
    Check(tree.Insert(Get, "/hello world", 1).has_value(), "decoded static registration");
    Check(tree.Insert(Get, "/files/:name", 2).has_value(), "one encoded file segment");
    Check(tree.Insert(Get, "/files/a/b", 3).has_value(), "two real path segments");
    Check(tree.Insert(Get, "/files/*", 4).has_value(), "fallback remainder");
    CheckEq(tree.Match(Get, "/hello%20world").handler, std::optional<std::size_t>{ 1 }, "percent decoded static match");
    auto encoded = tree.Match(Get, "/files/a%2Fb");
    Check(encoded.handler == 2 && encoded.params.Get("name") == "a/b", "encoded slash stays within a parameter");
    CheckEq(tree.Match(Get, "/files/a/b").handler, std::optional<std::size_t>{ 3 }, "real slash remains a separator");
    auto binary = tree.Match(Get, "/files/a%00b");
    CheckEq(binary.params.Get("name"), "a\0b"sv, "NUL capture uses explicit byte length");
    CheckEq(tree.Match(Get, "/files/a+b").params.Get("name"), "a+b"sv, "plus remains literal in paths");
    CheckEq(tree.Match(Get, "/files/%ZZ").params.Get("name"), "%ZZ"sv, "malformed escape stays literal");
    CheckEq(tree.Match(Get, "/files/%").params.Get("name"), "%"sv, "trailing percent stays literal");
    CheckEq(encoded.params.Get("name"), "a/b"sv, "earlier match owns its capture independently");
}

/**
 * @brief Match exactly at the decoded regex subject limit, skipping oversized subjects for lower priorities.
 */
auto regex_subjects() -> void {
    mhttp::detail::RouteTree tree;
    Check(tree.Insert(Get, "/{value:a+}", 1).has_value(), "bounded regex");
    std::string const exact = "/" + std::string(1024, 'a');
    std::string const over = exact + 'a';
    CheckEq(tree.Match(Get, exact).handler, std::optional<std::size_t>{ 1 }, "regex subject exactly at default limit");
    CheckEq(tree.Match(Get, over).status, mhttp::Status::NotFound, "oversized subject skips regex and returns 404");
    Check(tree.Insert(Get, "/:fallback", 2).has_value(), "unqualified fallback");
    CheckEq(tree.Match(Get, over).handler, std::optional<std::size_t>{ 2 }, "oversized regex subject still tries parameter");
    mhttp::detail::RouteTree small{ { .regex_subject_bytes = 2 } };
    Check(small.Insert(Get, "/{value:.*}", 1).has_value(), "configured subject bound");
    Check(small.Match(Get, "/%61%61").handler == 1, "bound applies to decoded bytes");
    Check(!small.Match(Get, "/aaa").handler, "configured subject bound exceeded");
    Check(small.Match(Get, "/").handler == 1, "regex can match empty final segment");
}

/**
 * @brief Exercise long paths without recursive matching or recursive node destruction.
 */
auto deep_paths() -> void {
    mhttp::detail::RouteTree tree;
    std::string path;
    for (unsigned index{}; index < 3000; ++index) { path += "/a"; }
    Check(tree.Insert(Get, path, 1).has_value(), "deep static registration");
    CheckEq(tree.Match(Get, path).handler, std::optional<std::size_t>{ 1 }, "deep static match");
    path += "/b";
    CheckEq(tree.Match(Get, path).status, mhttp::Status::NotFound, "deep failure backtracks iteratively");
}

/**
 * @brief Share an immutable tree between independent matching threads and keep captures isolated.
 */
auto concurrent_matching() -> void {
    mhttp::detail::RouteTree tree;
    Check(tree.Insert(Get, "/items/{id:[0-9]+}", 1).has_value(), "concurrent route");
    std::vector<std::jthread> workers;
    for (unsigned worker{}; worker < 4; ++worker) {
        workers.emplace_back([&tree, worker] {
            for (unsigned index{}; index < 100; ++index) {
                auto const id = worker * 100 + index;
                auto match = tree.Match(Get, std::format("/items/{}", id));
                Check(match.handler == 1 && match.params.Get<unsigned>("id") == id, "concurrent captures stay request-local");
            }
        });
    }
}

/**
 * @brief Run bounded routing-tree scenarios with optional name filters.
 * @param argc Argument count.
 * @param argv Borrowed command-line arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "segment matching", segment_matching },
        { "branch backtracking", branch_backtracking },
        { "method dispatch", method_dispatch },
        { "encoded matching", encoded_matching },
        { "regex subject limits", regex_subjects },
        { "deep paths", deep_paths },
        { "concurrent matching", concurrent_matching },
    });
}
