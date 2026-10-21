/**
 * @file multipart_test.cpp
 * @brief Verify owned multipart encoding, safe metadata escaping, boundary conflict handling, and atomic request mutation.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Match independently specified wire bytes and preserve owned field/file order through decoding.
 */
auto multipart_encoding() -> void {
    mhttp::FormData form;
    form.Add({ .name = "field", .content = "first" });
    form.Add({ .name = "file", .content = std::string{ "a\0b\r\n"sv }, .filename = "a.bin" });
    form.Add({ .name = "field", .content = "second", .content_type = "text/plain; charset=utf-8" });
    auto encoded = form.Encode("explicit-boundary");
    if (!Check(encoded.has_value(), "encode an explicit valid boundary")) { return; }
    auto expected = std::string{ "--explicit-boundary\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\nfirst\r\n"
        "--explicit-boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\na\0b\r\n\r\n"
        "--explicit-boundary\r\nContent-Disposition: form-data; name=\"field\"\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n\r\nsecond\r\n--explicit-boundary--\r\n"sv };
    CheckEq(encoded->body, expected, "wire bytes include exact disposition, inferred file type, binary bytes, and framing CRLFs");
    CheckEq(encoded->content_type, "multipart/form-data; boundary=\"explicit-boundary\"", "media declaration quotes the matching boundary");
    auto parsed = mhttp::FormData::Parse(encoded->body, encoded->content_type);
    Check(parsed && parsed->Size() == 3 && parsed->Count("field") == 2, "encoding retains interleaved duplicate fields");
    if (parsed) { CheckEq(parsed->Get("file")->content, "a\0b\r\n"sv, "content ending with CRLF is not shortened by framing"); }
    Check(form.Encode("b", { .body_bytes = form.Encode("b")->body.size() }).has_value(), "encoded body exactly at its limit is admitted");
    CheckEq(form.Encode("b", { .body_bytes = form.Encode("b")->body.size() - 1 }).error(), mhttp::Error::BodyTooLarge, "framing and metadata count toward output bytes");
    CheckEq(form.Encode("b", { .part_bytes = 4 }).error(), mhttp::Error::BodyTooLarge, "ordinary fields and file data share content admission");
    CheckEq(form.Encode("b", { .max_parts = 2 }).error(), mhttp::Error::BodyTooLarge, "encoding counts fields and files together");
    CheckEq(form.Encode("b", { .part_header_fields = 1 }).error(), mhttp::Error::InvalidHeaders, "inferred file Content-Type counts toward header admission");
    CheckEq(form.Encode("b", { .part_header_bytes = 16 }).error(), mhttp::Error::InvalidHeaders, "encoded disposition is subject to header byte admission");
    mhttp::FormData empty;
    auto encoded_empty = empty.Encode("b", { .max_parts = 0 });
    Check(encoded_empty && encoded_empty->body == "--b--\r\n", "empty form has one final boundary and no invented fields");
}

/**
 * @brief Keep quotes and CRLFs inside one disposition field and preserve literal percent sequences.
 */
auto multipart_escaping_and_boundaries() -> void {
    mhttp::FormData form;
    form.Add({ .name = "name\"\r\nX-Injected: yes", .content = "binary", .filename = "file\"\r\nY-Injected: yes\\.bin" });
    auto encoded = form.Encode("b");
    if (!Check(encoded.has_value(), "escape otherwise unsafe filename/field metadata")) { return; }
    Check(encoded->body.contains("name=\"name%22%0D%0AX-Injected: yes\""), "field quote and CRLF are encoded inside the parameter");
    Check(encoded->body.contains("filename=\"file%22%0D%0AY-Injected: yes\\\\.bin\""), "filename uses percent escapes plus backslash quoted-pairs");
    Check(!encoded->body.contains("\r\nX-Injected:") && !encoded->body.contains("\r\nY-Injected:"), "metadata cannot introduce additional MIME fields");
    auto parsed = mhttp::FormData::Parse(encoded->body, encoded->content_type);
    Check(parsed && parsed->Parts()[0].name == "name%22%0D%0AX-Injected: yes"
        && *parsed->Parts()[0].filename == "file%22%0D%0AY-Injected: yes\\.bin", "percent escapes remain literal metadata after decoding");
    for (auto boundary : { "bad@boundary", "trailing ", "\r\ninjected" }) {
        CheckEq(form.Encode(boundary).error(), mhttp::Error::InvalidRequest, "reject invalid explicit boundary alphabet");
    }
    CheckEq(form.Encode(std::string(71, 'b')).error(), mhttp::Error::InvalidRequest, "reject oversized explicit boundaries before copying them");
    mhttp::FormData conflict;
    conflict.Add({ .name = "data", .content = "prefix\r\n--b-not-a-safe-line\r\nsuffix" });
    CheckEq(conflict.Encode("b").error(), mhttp::Error::InvalidRequest, "explicit boundary cannot prefix a content line");
    auto generated = conflict.Encode();
    Check(generated && mhttp::FormData::Parse(generated->body, generated->content_type)->Get("data")->content == conflict.Get("data")->content, "generated boundary is checked against the actual content");
    auto second = conflict.Encode();
    Check(generated && second && generated->content_type != second->content_type, "default boundaries are distinct for independently encoded forms");
}

/**
 * @brief Preserve prior request state on construction errors and own every byte before asynchronous Send.
 */
auto multipart_request_mutation() -> void {
    mhttp::client::Request request{ mhttp::Method::Post, "/upload" };
    Check(request.SetContent("previous", "text/plain").has_value(), "set original request state");
    mhttp::FormData invalid;
    invalid.Add({ .name = "file", .content = "new", .filename = "a.bin", .content_type = "text/plain\r\nX: injected" });
    auto rejected = request.SetMultipart(invalid);
    Check(!rejected && rejected.error() == mhttp::Error::InvalidHeaders
        && request.Body() == "previous" && request.ContentType() == "text/plain", "metadata failure leaves body and Content-Type unchanged");
    mhttp::FormData large;
    large.Add({ .name = "field", .content = "12345" });
    auto bounded = request.SetMultipart(large, "b", { .part_bytes = 4 });
    Check(!bounded && bounded.error() == mhttp::Error::BodyTooLarge && request.Body() == "previous", "size rejection is atomic");
    Check(!request.SetMultipart(large, "bad@boundary") && request.Body() == "previous", "boundary rejection is atomic");
    {
        mhttp::FormData owned;
        owned.Add({ .name = "file", .content = std::string{ "owned\0data"sv }, .filename = "owned.bin" });
        Check(request.SetMultipart(owned).has_value(), "materialize outgoing multipart before source destruction");
    }
    auto parsed = mhttp::FormData::Parse(request.Body(), request.ContentType());
    Check(parsed && parsed->Get("file")->content == "owned\0data"sv, "outgoing request retains independent serialized content");
    CheckEq(request.Headers().Count("Content-Type"), std::size_t{ 1 }, "builder replaces prior media declaration with one matching field");
    CheckEq(request.Target(), "/upload", "multipart construction preserves the request target");
}

/**
 * @brief Run client multipart construction acceptance with a bounded watchdog.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "multipart encoded wire bytes", multipart_encoding },
        { "multipart metadata and boundary safety", multipart_escaping_and_boundaries },
        { "multipart request atomic ownership", multipart_request_mutation },
    });
}
