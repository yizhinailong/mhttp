/**
 * @file query_params_test.cpp
 * @brief Verify query parsing, ordered encoding, strict conversion, and target composition.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Verify first-separator splitting, repeated values, byte decoding, and name sensitivity.
 */
auto parse_queries() -> void {
    auto params = mhttp::QueryParams::Parse("?tag=one&middle=x&tag=two&flag&empty=&eq=a=b&Tag=upper");
    CheckEq(params.Size(), std::size_t{ 7 }, "count all parsed fields");
    CheckEq(params.Values("tag"), std::vector{ "one"sv, "two"sv }, "repeated values preserve order");
    CheckEq(params.Get("tag"), "one"sv, "lookup returns first value");
    CheckEq(params.Get("Tag"), "upper"sv, "lookup is case-sensitive");
    Check(params.Has("flag"), "key without equals is present");
    Check(params.Get("flag").empty(), "key without equals has an empty value");
    Check(params.Has("empty"), "explicit empty value is present");
    CheckEq(params.Get("eq"), "a=b"sv, "split only at the first equals");
    CheckEq(params.Get("missing", "default"), "default"sv, "missing value uses fallback");
    auto decoded = mhttp::QueryParams::Parse("n=a+b%2Bc&%E5%90%8D=%E5%80%BC&nul=a%00b&bad=%ZZ");
    CheckEq(decoded.Get("n"), "a b+c"sv, "query decoding restores spaces and literal plus");
    CheckEq(decoded.Get("\xE5\x90\x8D"), "\xE5\x80\xBC"sv, "UTF-8 parameter name and value");
    CheckEq(decoded.Get("nul"), "a\0b"sv, "query values preserve embedded NUL");
    CheckEq(decoded.Get("bad"), "%ZZ"sv, "malformed escape remains literal");
    for (auto input : { "", "?", "&&", " ", " = =", "\t=ignored" }) {
        Check(mhttp::QueryParams::Parse(input).Empty(), std::format("ignore empty query {:?}", input));
    }
    auto empty_name = mhttp::QueryParams::Parse("=value");
    CheckEq(empty_name.Get(""), "value"sv, "explicit empty names are supported");
}

/**
 * @brief Verify encoding order, independent storage, replacements, and binary round trips.
 */
auto ordered_encoding() -> void {
    mhttp::QueryParams params;
    params.Add("z", "last alphabetically");
    params.Add("a", "first alphabetically");
    params.Add("z", "+&=");
    CheckEq(params.Encode(), "z=last+alphabetically&a=first+alphabetically&z=%2B%26%3D"sv,
        "encode by insertion order rather than key order");
    auto copy = params;
    copy.Set("z", "replacement");
    CheckEq(copy.Encode(), "a=first+alphabetically&z=replacement"sv, "Set appends after deleting matches");
    CheckEq(params.Count("z"), std::size_t{ 2 }, "copy mutation leaves original values");
    auto const& first = *copy.begin();
    copy.Set(first.first, first.second);
    CheckEq(copy.Encode(), "z=replacement&a=first+alphabetically"sv, "self-referential Set owns input");
    copy.Add(copy.begin()->first, copy.begin()->second);
    CheckEq(copy.Erase(copy.begin()->first), std::size_t{ 2 }, "self-referential Erase removes all matches");
    CheckEq(copy.Get("a"), "first alphabetically"sv, "other keys survive erasure");
    copy.Clear();
    Check(copy.Empty() && copy.begin() == copy.end(), "clear resets storage and iteration");
    params.Clear();
    std::string bytes;
    for (unsigned byte{}; byte <= 255; ++byte) { bytes.push_back(static_cast<char>(byte)); }
    params.Add(bytes, bytes);
    params.Add("", "");
    auto const round_trip = mhttp::QueryParams::Parse(params.Encode());
    CheckEq(round_trip.Get(bytes), bytes, "all binary bytes round-trip in names and values");
    CheckEq(round_trip.Size(), std::size_t{ 2 }, "empty named parameter round-trips");
}

/**
 * @brief Verify strict typed values return absence instead of throwing on malformed input.
 */
auto typed_query_values() -> void {
    auto const params = mhttp::QueryParams::Parse(
        "number=-17&yes=true&no=false&one=1&zero=0&capital=True&text=abc&overflow=18446744073709551616"
        "&junk=12x&plus=%2B1&fraction=1.25&nan=nan&infinite=inf&empty=&repeated=5&repeated=7");
    CheckEq(params.Get<int>("number"), std::optional{ -17 }, "signed integer conversion");
    CheckEq(params.Get<bool>("yes"), std::optional{ true }, "bool true");
    CheckEq(params.Get<bool>("no"), std::optional{ false }, "bool false");
    CheckEq(params.Get<bool>("one"), std::optional{ true }, "bool numeric one");
    CheckEq(params.Get<bool>("zero"), std::optional{ false }, "bool numeric zero");
    Check(!params.Get<bool>("capital"), "bool spelling is exact");
    Check(!params.Get<int>("text"), "nonnumeric text returns absence");
    Check(!params.Get<std::uint64_t>("overflow"), "overflow returns absence");
    Check(!params.Get<unsigned>("number"), "unsigned conversion rejects negatives");
    Check(!params.Get<int>("junk"), "trailing junk is not consumed partially");
    Check(!params.Get<int>("plus"), "leading plus is rejected");
    Check(!params.Get<int>("empty"), "empty value is not zero");
    Check(!params.Get<int>("missing"), "missing value returns absence");
    CheckEq(params.Get<int>("repeated"), std::optional{ 5 }, "typed lookup uses first value");
    CheckEq(params.Get<double>("fraction"), std::optional{ 1.25 }, "floating point conversion");
    Check(!params.Get<double>("nan") && !params.Get<double>("infinite"), "reject nonfinite values");
}

/**
 * @brief Verify appending query parameters preserves existing targets and fragments.
 */
auto append_to_targets() -> void {
    auto const params = mhttp::QueryParams::Parse("a=one&b=two");
    constexpr std::pair<std::string_view, std::string_view> TARGETS[]{
        { "/path", "/path?a=one&b=two" }, { "/path?", "/path?a=one&b=two" },
        { "/path?z=3", "/path?z=3&a=one&b=two" }, { "/path?z=3&", "/path?z=3&a=one&b=two" },
        { "/path#fragment", "/path?a=one&b=two#fragment" },
        { "/path?z=3#fragment?x", "/path?z=3&a=one&b=two#fragment?x" },
    };
    for (auto const& [target, expected] : TARGETS) {
        CheckEq(params.AppendTo(target), expected, std::format("append to {:?}", target));
        CheckEq(mhttp::QueryParams{}.AppendTo(target), target, "empty parameters leave target unchanged");
    }
}

/**
 * @brief Run query parameter cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "parse queries", parse_queries },
        { "ordered query encoding", ordered_encoding },
        { "typed query values", typed_query_values },
        { "append to targets", append_to_targets },
    });
}
