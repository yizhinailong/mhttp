/**
 * @file percent_encoding_test.cpp
 * @brief Verify URI byte sets, malformed escapes, binary data, and contextual plus handling.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::CheckEq;
using mhttp::UrlDecode;
using mhttp::UrlEncode;
using mhttp::UrlEncoding;
using namespace std::string_view_literals;

/**
 * @brief Verify the exact safe sets, uppercase escapes, and UTF-8 byte handling.
 */
auto encoding_sets() -> void {
    CheckEq(UrlEncode("AZaz09-_.!~*'()"), "AZaz09-_.!~*'()"sv, "component safe set");
    CheckEq(UrlEncode(";/?:@&=+$,#"), "%3B%2F%3F%3A%40%26%3D%2B%24%2C%23"sv,
        "component separators are escaped");
    CheckEq(UrlEncode("a b+c", UrlEncoding::PathSegment), "a%20b%2Bc"sv, "path spaces and plus");
    CheckEq(UrlEncode("a b+c", UrlEncoding::QueryComponent), "a+b%2Bc"sv, "query spaces and plus");
    CheckEq(UrlEncode("\xE4\xB8\xAD\xE6\x96\x87"), "%E4%B8%AD%E6%96%87"sv, "UTF-8 bytes");
    CheckEq(UrlEncode("http://host/a b?q=x&y=z#f", UrlEncoding::Uri),
        "http://host/a%20b?q=x&y=z#f"sv, "complete URI preserves delimiters");
    CheckEq(UrlEncode("\0\xFF\x7F"sv), "%00%FF%7F"sv, "NUL, high byte, and DEL are escaped");
}

/**
 * @brief Verify malformed and truncated escape sequences remain literal in every mode.
 */
auto malformed_escapes() -> void {
    for (auto input : { ""sv, "%"sv, "%2"sv, "%ZZ"sv, "%-1"sv, "%+5"sv, "% 5"sv,
                       "%u0041"sv, "%%"sv, "tail%"sv, "tail%A"sv }) {
        for (auto mode : { UrlEncoding::PathSegment, UrlEncoding::QueryComponent, UrlEncoding::Uri }) {
            CheckEq(UrlDecode(input, mode), input, std::format("malformed escape {:?}", input));
        }
    }
    CheckEq(UrlDecode("a%20b%c"), "a b%c"sv, "valid and truncated escapes can coexist");
    CheckEq(UrlDecode("%ZZ%41%"), "%ZZA%"sv, "decoding continues after a malformed triplet");
    std::array<char, 1> const one{ '%' };
    std::array<char, 2> const two{ '%', 'F' };
    CheckEq(UrlDecode({ one.data(), one.size() }), "%"sv, "one-byte nonterminated input");
    CheckEq(UrlDecode({ two.data(), two.size() }), "%F"sv, "two-byte nonterminated input");
}

/**
 * @brief Verify decoding mode, escape casing, embedded NULs, and complete-URI preservation.
 */
auto decoding_context() -> void {
    CheckEq(UrlDecode("a+b%2Bc", UrlEncoding::QueryComponent), "a b+c"sv, "query plus is a space");
    CheckEq(UrlDecode("a+b%2Bc"), "a+b+c"sv, "path plus remains literal");
    CheckEq(UrlDecode("%e4%b8%ad%e6%96%87"), "\xE4\xB8\xAD\xE6\x96\x87"sv, "lowercase UTF-8 escapes");
    CheckEq(UrlDecode("a%00b"), "a\0b"sv, "decoded NUL preserves full length");
    CheckEq(UrlDecode("%2f%3F%23%2B%40%26%3B%2C%3A%3D%24", UrlEncoding::Uri),
        "%2f%3F%23%2B%40%26%3B%2C%3A%3D%24"sv, "URI reserved escapes retain spelling");
    CheckEq(UrlDecode("%2f%3F%23"), "/?#"sv, "component decoding restores separators");
    CheckEq(UrlDecode("%2D%20", UrlEncoding::Uri), "- "sv, "URI unreserved escapes decode");
}

/**
 * @brief Verify every possible byte and mixed binary strings round-trip through every mode.
 */
auto binary_round_trips() -> void {
    std::string bytes;
    for (unsigned byte{}; byte <= 255; ++byte) {
        bytes.push_back(static_cast<char>(byte));
    }
    for (auto mode : { UrlEncoding::PathSegment, UrlEncoding::QueryComponent, UrlEncoding::Uri }) {
        CheckEq(UrlDecode(UrlEncode(bytes, mode), mode), bytes, "all 256 byte values round-trip");
        for (std::size_t count{}; count <= bytes.size(); ++count) {
            std::string_view input{ bytes.data(), count };
            CheckEq(UrlDecode(UrlEncode(input, mode), mode), input,
                std::format("binary prefix of {} bytes round-trips", count));
        }
    }
}

/**
 * @brief Run percent-encoding cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "encoding sets", encoding_sets },
        { "malformed escapes", malformed_escapes },
        { "decoding context", decoding_context },
        { "binary round trips", binary_round_trips },
    });
}
