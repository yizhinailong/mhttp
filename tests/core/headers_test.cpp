/**
 * @file headers_test.cpp
 * @brief Verify ordered multi-value headers, validated mutation, ownership, and alias safety.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Observe header iteration independently of internal storage types.
 * @param headers Borrowed collection.
 * @return Copied fields in the observed wire order.
 */
auto copy_fields(mhttp::Headers const& headers) -> std::vector<std::pair<std::string, std::string>> {
    return { headers.begin(), headers.end() };
}

/**
 * @brief Verify empty, repeated, case-insensitive, and ordered header behavior.
 */
auto ordered_headers() -> void {
    mhttp::Headers headers;
    Check(headers.Empty(), "new headers are empty");
    CheckEq(headers.Size(), std::size_t{}, "new headers have zero fields");
    Check(headers.begin() == headers.end(), "empty iteration");
    CheckEq(headers.Get("missing", "fallback"), "fallback"sv, "missing field returns default");
    Check(!headers.Has("missing"), "missing field not present");
    Check(headers.Values("missing").empty(), "missing field has no values");
    Check(headers.Add("X-A", "one").has_value(), "add first field");
    Check(headers.Add("X-B", "middle").has_value(), "add interleaved field");
    Check(headers.Add("x-a", "two").has_value(), "add repeated field with different casing");
    Check(headers.Add("X-Empty", "").has_value(), "empty values are valid");
    Check(headers.Has("x-empty"), "empty field is still present");
    CheckEq(headers.Get("x-a"), "one"sv, "lookup returns first matching value");
    CheckEq(headers.Count("X-A"), std::size_t{ 2 }, "count ignores name casing");
    CheckEq(headers.Values("X-a"), std::vector{ "one"sv, "two"sv }, "values retain insertion order");
    CheckEq(copy_fields(headers), std::vector<std::pair<std::string, std::string>>{
        { "X-A", "one" }, { "X-B", "middle" }, { "x-a", "two" }, { "X-Empty", "" }
    }, "iteration preserves interleaving and original spelling");
    CheckEq(headers.Erase("x-A"), std::size_t{ 2 }, "erase removes all matching names");
    CheckEq(copy_fields(headers), std::vector<std::pair<std::string, std::string>>{
        { "X-B", "middle" }, { "X-Empty", "" }
    }, "erasure preserves other field order");
    CheckEq(headers.Erase("absent"), std::size_t{}, "erasing absent name is a no-op");
    headers.Clear();
    Check(headers.Empty(), "clear removes all fields");
    Check(headers.begin() == headers.end(), "iteration after clear is empty");
}

/**
 * @brief Verify replacement position, independent copies, and views passed back into mutation.
 */
auto replacement_and_ownership() -> void {
    mhttp::Headers headers;
    Check(headers.Add("X-A", "one").has_value(), "add first");
    Check(headers.Add("X-B", "middle").has_value(), "add second");
    Check(headers.Add("x-a", "two").has_value(), "add repeated");
    Check(headers.Set("X-a", "replacement").has_value(), "replace all case-insensitive matches");
    CheckEq(copy_fields(headers), std::vector<std::pair<std::string, std::string>>{
        { "X-B", "middle" }, { "X-a", "replacement" }
    }, "Set appends after removing every old value");
    auto copied = headers;
    Check(copied.Set("X-B", "changed").has_value(), "mutate copied headers");
    CheckEq(headers.Get("X-B"), "middle"sv, "copy owns independent values");
    auto const& first = *headers.begin();
    Check(headers.Set(first.first, first.second).has_value(), "Set accepts views into its own storage");
    CheckEq(headers.Get("X-B"), "middle"sv, "self-referential Set retains bytes");
    Check(headers.Add(headers.begin()->first, headers.begin()->second).has_value(), "self-referential Add");
    CheckEq(headers.Count("X-a"), std::size_t{ 2 }, "self-referential Add duplicates the intended field");
    CheckEq(headers.Erase(headers.begin()->first), std::size_t{ 2 }, "self-referential Erase owns its lookup key");
    CheckEq(headers.Get("X-B"), "middle"sv, "remaining field survives self-referential erasure");
}

/**
 * @brief Reject response-splitting bytes before changing an existing collection.
 */
auto invalid_headers() -> void {
    mhttp::Headers headers;
    Check(headers.Add("X-Test", "original").has_value(), "prepare original field");
    auto const original = copy_fields(headers);
    for (auto name : { ""sv, "X Name"sv, "X:Name"sv, "X\rName"sv, "X\nName"sv, "X\0Name"sv }) {
        auto const added = headers.Add(name, "value");
        auto const set = headers.Set(name, "value");
        if (Check(!added && !set, std::format("reject invalid name {:?}", name))) {
            CheckEq(added.error(), mhttp::Error::InvalidHeaders, "Add reports header validation error");
            CheckEq(set.error(), mhttp::Error::InvalidHeaders, "Set reports header validation error");
        }
        CheckEq(copy_fields(headers), original, "invalid name leaves all fields untouched");
    }
    for (auto value : { " injected"sv, "trailing "sv, "\tvalue"sv, "value\t"sv, "a\0b"sv,
                        "a\r\nInjected: yes"sv, "a\nb"sv, "a\x7F"sv, "a\x01"sv }) {
        Check(!headers.Add("X-Test", value), std::format("reject Add value {:?}", value));
        Check(!headers.Set("x-test", value), std::format("reject Set value {:?}", value));
        CheckEq(copy_fields(headers), original, "invalid value leaves all fields untouched");
    }
    Check(headers.Set(mhttp::header::CONTENT_TYPE, "text/plain; charset=utf-8").has_value(), "common header constant");
    Check(headers.Add("X-Binary", "\x80\xFF"sv).has_value(), "obs-text bytes are valid field content");
}

/**
 * @brief Run header cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "ordered headers", ordered_headers },
        { "header replacement and ownership", replacement_and_ownership },
        { "invalid headers", invalid_headers },
    });
}
