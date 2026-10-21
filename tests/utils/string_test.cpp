/**
 * @file string_test.cpp
 * @brief Verify HTTP string primitives, long keys, strict integers, and every byte class.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Verify equality, transparent lookup, embedded NULs, and large iterative hashes.
 */
auto case_insensitive_keys() -> void {
    using namespace mhttp::utils;
    Check(iequals("Content-Type", "cONTENT-tYPE"), "header names ignore ASCII case");
    Check(!iequals("Host", "Hosts"), "different lengths differ");
    Check(!iequals("Host", "Post"), "different letters differ");
    Check(iequals("", ""), "empty keys compare equal");
    Check(!iequals("a\0b"sv, "a\0c"sv), "embedded NUL does not terminate equality");
    CheckEq(ToLower("A\0Z\xC0"sv), "a\0z\xC0"sv, "lowercasing preserves NUL and non-ASCII");
    std::unordered_map<std::string, int, CaseInsensitiveHash, CaseInsensitiveEqual> headers;
    headers.emplace("X-Name", 42);
    CheckEq(headers.at("x-NAME"), 42, "folded map lookup succeeds");
    Check(headers.find("X-name"sv) != headers.end(), "transparent lookup accepts a view");
    std::string upper(1024 * 1024, 'A');
    std::string lower(upper.size(), 'a');
    Check(iequals(upper, lower), "1 MiB keys compare without recursion");
    CheckEq(CaseInsensitiveHash{}(upper), CaseInsensitiveHash{}(lower), "equal long keys hash equally");
}

/**
 * @brief Verify OWS-only trimming, preserved empty segments, and borrowed split storage.
 */
auto trim_and_split() -> void {
    using namespace mhttp::utils;
    for (auto text : { "abc"sv, " abc "sv, "\t abc \t"sv }) {
        CheckEq(trim_ows(text), "abc"sv, std::format("trim input {:?}", text));
    }
    Check(trim_ows("").empty(), "empty trim stays empty");
    Check(trim_ows(" \t ").empty(), "OWS-only input becomes empty");
    CheckEq(trim_ows("\r\nabc\v\f"), "\r\nabc\v\f"sv, "non-OWS controls are preserved");
    Check(split("", ',').empty(), "empty split produces no segments");
    CheckEq(split("a,b,c", ','), std::vector{ "a"sv, "b"sv, "c"sv }, "basic split");
    CheckEq(split(",a,,", ','), std::vector{ ""sv, "a"sv, ""sv, ""sv }, "empty segments survive");
    CheckEq(split("/users/", '/'), std::vector{ ""sv, "users"sv, ""sv }, "trailing route slash survives");
    CheckEq(split(" a , \t, b ", ',', true), std::vector{ "a"sv, "b"sv }, "compressed split trims OWS");
    std::string storage{ "a,b" };
    auto parts = split(storage, ',');
    storage[0] = 'z';
    CheckEq(parts.front(), "z"sv, "split segments borrow original storage");
}

/**
 * @brief Check integer endpoints, signs, overflow, and unconsumed bytes.
 */
auto strict_integers() -> void {
    using mhttp::utils::parse_integer;
    CheckEq(parse_integer<int>("42"), std::optional{ 42 }, "decimal integer");
    CheckEq(parse_integer<int>("-17"), std::optional{ -17 }, "negative signed integer");
    CheckEq(parse_integer<int>("001"), std::optional{ 1 }, "leading decimal zeroes");
    CheckEq(parse_integer<unsigned>("ff", 16), std::optional{ 255U }, "explicit hexadecimal base");
    for (auto input : { ""sv, "+1"sv, " 1"sv, "1 "sv, "1x"sv, "x1"sv, "0x2e"sv,
                       "1.5"sv, "12345678901234567890123456"sv, "1\0"sv }) {
        Check(!parse_integer<int>(input), std::format("reject invalid integer {:?}", input));
    }
    Check(!parse_integer<unsigned>("-1"), "unsigned integer rejects negative sign");
    Check(!parse_integer<int>("1", 1), "reject base below valid range");
    Check(!parse_integer<int>("1", 37), "reject base above valid range");
    CheckEq(parse_integer<std::int8_t>("127"), std::optional<std::int8_t>{ 127 }, "signed maximum");
    CheckEq(parse_integer<std::int8_t>("-128"), std::optional<std::int8_t>{ -128 }, "signed minimum");
    Check(!parse_integer<std::int8_t>("128"), "signed maximum plus one fails");
    Check(!parse_integer<std::int8_t>("-129"), "signed minimum minus one fails");
    CheckEq(parse_integer<std::uint8_t>("255"), std::optional<std::uint8_t>{ 255 }, "unsigned maximum");
    Check(!parse_integer<std::uint8_t>("256"), "unsigned maximum plus one fails");
    CheckEq(parse_integer<std::uint64_t>("18446744073709551615"),
        std::optional{ std::numeric_limits<std::uint64_t>::max() }, "64-bit maximum");
    Check(!parse_integer<std::uint64_t>("18446744073709551616"), "64-bit overflow fails");
}

/**
 * @brief Exhaustively verify HTTP token and field-value byte classes.
 */
auto http_character_classes() -> void {
    using namespace mhttp::utils;
    constexpr auto TOKEN_BYTES = "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"sv;
    for (unsigned value{}; value <= 255; ++value) {
        char const byte = static_cast<char>(value);
        auto const message = std::format("classification of byte {}", value);
        CheckEq(is_token_char(byte), TOKEN_BYTES.contains(byte), message);
        std::string field{ 'x', byte, 'y' };
        CheckEq(is_field_value(field), value == 9 || (value >= 32 && value != 127), message);
        CheckEq(ascii_lower(byte), value >= 65 && value <= 90 ? static_cast<char>(value + 32) : byte,
            message);
    }
    Check(is_field_name("X-Custom_123"), "valid token name");
    Check(is_field_name("!#$%&'*+-.^_`|~"), "all token punctuation accepted");
    for (auto name : { ""sv, "a b"sv, "a:b"sv, "a@b"sv, "a\tb"sv, "a\rb"sv, "a\nb"sv, "a\0b"sv }) {
        Check(!is_field_name(name), std::format("reject field name {:?}", name));
    }
    Check(is_field_value(""), "empty field value allowed");
    Check(is_field_value("a b\tc"), "interior spaces and tabs allowed");
    for (auto value : { " x"sv, "x "sv, "\tx"sv, "x\t"sv, "\0"sv, "x\r\ny"sv }) {
        Check(!is_field_value(value), std::format("reject field value {:?}", value));
    }
}

/**
 * @brief Restore the process C and C++ locales after testing named locale changes.
 */
struct LocaleRestore {
    std::locale previous; ///< Original global locale, retained until scope exit.
    /**
     * @brief Restore before another case executes, including when a locale assertion throws.
     */
    ~LocaleRestore() { std::locale::global(previous); }
};

/**
 * @brief Keep HTTP classification and case folding ASCII-only after named global/setlocale changes.
 */
auto locale_independent_ascii() -> void {
    LocaleRestore restore;
    unsigned tested{};
    for (auto name : { "C", "C.UTF-8", "C.utf8", "en_US.UTF-8", "tr_TR.UTF-8",
                       "Turkish_Turkey.1254", ".1254", "" }) {
        std::optional<std::locale> candidate;
        try { candidate.emplace(name); }
        catch (std::runtime_error const&) { continue; } // Locale packages differ across platforms.
        std::locale::global(*candidate); // Named global locales also update the process C locale.
        ++tested;
        http_character_classes();
        Check(mhttp::utils::iequals("I-IDENTIFIER", "i-identifier"),
            std::format("ASCII I folds independently of locale {:?}", name));
        CheckEq(mhttp::utils::ToLower("I\xDD\xFD\xC0"sv), "i\xDD\xFD\xC0"sv,
            std::format("non-ASCII bytes survive folding in locale {:?}", name));
        CheckEq(mhttp::utils::CaseInsensitiveHash{}("IDENTIFIER"),
            mhttp::utils::CaseInsensitiveHash{}("identifier"), "locale-independent hash/equality agreement");
    }
    Check(tested > 0, "at least the required C locale was exercised");
}

/**
 * @brief Run the HTTP string utility cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "case-insensitive keys", case_insensitive_keys },
        { "trim and split", trim_and_split },
        { "strict integers", strict_integers },
        { "HTTP character classes", http_character_classes },
        { "locale-independent ASCII", locale_independent_ascii },
    });
}
