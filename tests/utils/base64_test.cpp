/**
 * @file base64_test.cpp
 * @brief Verify standard Base64 vectors, strict padding, and binary round trips.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Check RFC 4648 vectors and an HTTP Basic authentication credential.
 */
auto known_vectors() -> void {
    constexpr std::pair<std::string_view, std::string_view> VECTORS[]{
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
        { "Aladdin:open sesame", "QWxhZGRpbjpvcGVuIHNlc2FtZQ==" },
        { "\0\xFF\x80"sv, "AP+A" },
    };
    for (auto const& [plain, encoded] : VECTORS) {
        CheckEq(mhttp::Base64Encode(plain), encoded, std::format("encode {:?}", plain));
        auto const decoded = mhttp::Base64Decode(encoded);
        if (Check(decoded.has_value(), std::format("decode {:?}", encoded))) {
            CheckEq(*decoded, plain, "known decoded bytes");
        }
    }
}

/**
 * @brief Reject misplaced padding, nonzero pad bits, whitespace, and invalid bytes.
 */
auto malformed_base64() -> void {
    for (auto encoded : { "A"sv, "AA"sv, "AAA"sv, "A==="sv, "===="sv, "AA=A"sv,
                          "Zg==AAAA"sv, "AAA=AAAA"sv, "AB=="sv, "AAB="sv, "!AAA"sv,
                          "AA-A"sv, "AA_A"sv, "AA A"sv, "AA\nA"sv, "AAA\0"sv, "AAA\xFF"sv }) {
        Check(!mhttp::Base64Decode(encoded), std::format("reject Base64 {:?}", encoded));
    }
    for (unsigned byte{}; byte <= 255; ++byte) {
        char const value = static_cast<char>(byte);
        std::string candidate{ "AAA" };
        candidate.push_back(value);
        bool const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/="sv.contains(value);
        CheckEq(mhttp::Base64Decode(candidate).has_value(), alphabet,
            std::format("alphabet classification of byte {}", byte));
    }
}

/**
 * @brief Exercise every byte and all three padding lengths with growing binary inputs.
 */
auto binary_base64() -> void {
    std::string bytes;
    for (unsigned byte{}; byte <= 255; ++byte) {
        bytes.push_back(static_cast<char>(byte));
        auto const decoded = mhttp::Base64Decode(mhttp::Base64Encode(bytes));
        if (Check(decoded.has_value(), "encoded binary data decodes")) {
            CheckEq(*decoded, bytes, std::format("binary round trip of {} bytes", bytes.size()));
        }
    }
}

/**
 * @brief Run Base64 cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "known Base64 vectors", known_vectors },
        { "malformed Base64", malformed_base64 },
        { "binary Base64", binary_base64 },
    });
}
