/**
 * @file mime_test.cpp
 * @brief Verify media lookup boundaries, ASCII case folding, and caller-owned fallbacks.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

/**
 * @brief Select the final extension without interpreting URL query syntax or non-ASCII bytes.
 */
auto mime_extensions() -> void {
    using mhttp::test::CheckEq;
    CheckEq(mhttp::MimeType("/assets/ICON.SvG"), std::string_view{ "image/svg+xml" }, "ASCII extension comparison");
    CheckEq(mhttp::MimeType("archive.tar.gz"), std::string_view{ "application/gzip" }, "final extension wins");
    CheckEq(mhttp::MimeType("C:\\folder.png\\asset.JPEG"), std::string_view{ "image/jpeg" }, "Windows path separator");
    for (auto name : { "README", ".", "file.", "/folder.png/file", "file.png?x=1", "file.PN\xC7" }) {
        CheckEq(mhttp::MimeType(name), std::string_view{ "application/octet-stream" }, "unknown extension fallback");
    }
    std::string fallback{ "application/custom" };
    mhttp::test::Check(mhttp::MimeType("file.unknown", fallback).data() == fallback.data(), "fallback borrows caller storage");
    CheckEq(mhttp::MimeType("index.html"), std::string_view{ "text/html; charset=utf-8" }, "HTML default encoding");
    CheckEq(mhttp::MimeType("app.js"), std::string_view{ "text/javascript" }, "JavaScript media type");
}

/**
 * @brief Run media lookup cases.
 * @param argc Argument count.
 * @param argv Optional case filters.
 * @return Test process status.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, { { "MIME extensions", mime_extensions } });
}
