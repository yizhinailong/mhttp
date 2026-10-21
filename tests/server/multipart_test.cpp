/**
 * @file multipart_test.cpp
 * @brief Verify multipart MIME boundaries, owned metadata/content, malformed input, limits, and linear admission scanning.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Own a one-part raw MIME representation independently of the production encoder.
 * @param content Owned arbitrary binary part bytes.
 * @param extra_headers Borrowed additional complete header lines.
 * @return Complete wire body with the fixed boundary b.
 */
auto multipart_wire(std::string content, std::string_view extra_headers = {}) -> std::string {
    return "--b\r\nContent-Disposition: form-data; name=\"field\"\r\n" + std::string{ extra_headers }
        + "\r\n" + content + "\r\n--b--\r\n";
}

/**
 * @brief Accept legal quoted/unquoted boundary declarations and reject ambiguous or malformed parameters.
 */
auto multipart_boundaries() -> void {
    for (auto type : { "multipart/form-data; boundary=b", "Multipart/Form-Data; boundary=\"b\"",
                       "multipart/form-data; charset=utf-8; BOUNDARY=\"b\"; note=\"a;b\\\"c\"" }) {
        auto boundary = mhttp::FormData::ParseBoundary(type);
        Check(boundary && *boundary == "b", "parse case-insensitive media/parameter names with quoted separators");
    }
    Check(mhttp::FormData::ParseBoundary("multipart/form-data; boundary=\"a b/?:=\"").has_value(), "quoted boundary admits the RFC punctuation alphabet");
    Check(mhttp::FormData::ParseBoundary("multipart/form-data; boundary=" + std::string(70, 'b')).has_value(), "seventy-byte boundary is accepted");
    for (auto type : { "", "text/plain; boundary=b", "multipart/form-data", "multipart/form-data; boundary=\"\"",
                       "multipart/form-data; boundary=\"b \"", "multipart/form-data; boundary=\"a@b\"" }) {
        auto boundary = mhttp::FormData::ParseBoundary(type);
        Check(!boundary && boundary.error() == mhttp::Error::InvalidRequest, "reject missing, wrong-type, or invalid boundary");
    }
    CheckEq(mhttp::FormData::ParseBoundary("multipart/form-data; boundary=" + std::string(71, 'b')).error(),
        mhttp::Error::InvalidRequest, "seventy-one-byte boundary is rejected");
    for (auto type : { "multipart/form-data; boundary=b; BOUNDARY=b", "multipart/form-data; boundary=\"b",
                       "multipart/form-data; boundary=\"b\"junk", "multipart/form-data; boundary=b;",
                       "multipart/form-data; boundary=a/b", "multipart/form-data; boundary=b\r\nX: injected" }) {
        auto boundary = mhttp::FormData::ParseBoundary(type);
        Check(!boundary && boundary.error() == mhttp::Error::InvalidHeaders, "reject ambiguous/invalid MIME parameter syntax");
    }
    std::string parameters = "multipart/form-data; boundary=b";
    for (unsigned index{}; index < 31; ++index) { parameters += "; p" + std::to_string(index) + "=x"; }
    Check(mhttp::FormData::ParseBoundary(parameters).has_value(), "thirty-two bounded MIME parameters are admitted");
    CheckEq(mhttp::FormData::ParseBoundary(parameters + "; overflow=x").error(), mhttp::Error::InvalidHeaders,
        "parameter count includes ignored extension parameters");
    std::string maximum = "multipart/form-data; boundary=b; note=\"";
    maximum.append(64 * 1024 - maximum.size() - 1, 'x');
    maximum += '"';
    Check(mhttp::FormData::ParseBoundary(maximum).has_value(), "MIME declaration exactly at its text limit is accepted");
    maximum.insert(maximum.size() - 1, 1, 'x');
    CheckEq(mhttp::FormData::ParseBoundary(maximum).error(), mhttp::Error::InvalidHeaders, "oversized MIME declaration is rejected before parameter allocation");
}

/**
 * @brief Preserve part order, duplicate fields/files, quoted pairs, binary bytes, padding, preamble, and epilogue.
 */
auto multipart_owned_parts() -> void {
    std::string body = "preamble --b is not at a line boundary\r\n--b \t\r\n"
        "Content-Disposition: form-data; name=\"a;\\\"b\"\r\n\r\nfirst\r\n--b\r\n"
        "Content-Disposition: form-data; name=\"files\"; filename=\"a\\\"b\\\\c.bin\"\r\n"
        "Content-Type: application/octet-stream\r\nX-Extra: ignored\r\n\r\n";
    auto binary = std::string{ "x\0y\r\n--bX\r\n--b--not-closing\r\ntail\r\n"sv };
    body += binary;
    body += "\r\n--b\r\nContent-Disposition: form-data; name=files; filename=\"\"\r\n\r\n\r\n"
        "--b\r\nContent-Disposition: form-data; name=\"a;\\\"b\"\r\n\r\nsecond\r\n--b-- \t\r\nepilogue";
    auto form = mhttp::FormData::Parse(body, "multipart/form-data; boundary=b");
    if (!Check(form.has_value(), "parse multipart preamble, repeated files, and transport padding")) { return; }
    CheckEq(form->Size(), std::size_t{ 4 }, "count fields and files together");
    CheckEq(form->Count("a;\"b"), std::size_t{ 2 }, "escaped quote does not terminate a disposition parameter");
    CheckEq(form->Count("files"), std::size_t{ 2 }, "preserve multiple files with the same field name");
    CheckEq(form->Parts()[0].content, "first"sv, "first ordinary field remains first");
    Check(!form->Parts()[0].filename, "ordinary field has no filename metadata");
    CheckEq(*form->Parts()[1].filename, "a\"b\\c.bin", "quoted pairs decode quotes/backslashes once");
    CheckEq(form->Parts()[1].content, binary, "false delimiter prefixes and binary content remain intact");
    CheckEq(form->Parts()[1].content_type, "application/octet-stream", "retain optional part media type");
    Check(form->Parts()[2].filename && form->Parts()[2].filename->empty() && form->Parts()[2].content.empty(), "empty uploaded filename differs from absent filename");
    CheckEq(form->Parts()[3].content, "second"sv, "interleaved repeated field order is retained");
    Check(!form->Get("Files") && form->Get("files"), "field lookup remains case-sensitive");
    body.clear();
    CheckEq(form->Get("files")->content, binary, "parsed form does not borrow source body storage");
    auto empty = mhttp::FormData::Parse("--b--", "multipart/form-data; boundary=b");
    Check(empty && empty->Size() == 0, "accept explicitly closed empty forms without final CRLF");
    auto percent = mhttp::FormData::Parse("--b\r\nContent-Disposition: form-data; name=\"%22\"; filename=\"%2Ftmp\"\r\n\r\nx\r\n--b--", "multipart/form-data; boundary=b");
    Check(percent && percent->Get("%22") && *percent->Get("%22")->filename == "%2Ftmp", "percent escapes stay literal metadata, without implicit path decoding");
}

/**
 * @brief Reject malformed framing/dispositions and apply equal byte/count bounds to every part kind.
 */
auto multipart_rejection_and_limits() -> void {
    constexpr auto TYPE = "multipart/form-data; boundary=b";
    for (auto body : { "", "--b\n", "--b\r\nContent-Disposition: form-data; name=x\r\n\r\nno closing delimiter",
                       "prefix--b\r\nContent-Disposition: form-data; name=x\r\n\r\nx\r\n--b--bad" }) {
        Check(!mhttp::FormData::Parse(body, TYPE), "reject missing/truncated/misplaced boundary framing");
    }
    for (auto header : { "Content-Disposition: attachment; name=x", "Content-Disposition: form-data",
                         "Content-Disposition: form-data; name=x; NAME=y", "Content-Disposition: form-data; name=\"unterminated",
                         "Content-Disposition: form-data; name=x\r\n folded", "Content-Disposition : form-data; name=x",
                         "Content-Disposition: form-data; name=x\r\nContent-Disposition: form-data; name=y",
                         "Bad Field: value", "Content-Disposition: form-data; name=x\r\nContent-Type: text/plain; bad",
                         "Content-Disposition: form-data; name=x\r\nContent-Type: text/plain\r\nContent-Type: text/plain" }) {
        auto result = mhttp::FormData::Parse("--b\r\n" + std::string{ header } + "\r\n\r\nx\r\n--b--", TYPE);
        Check(!result && result.error() == mhttp::Error::InvalidHeaders, "bad or ambiguous part headers are recoverable");
    }
    for (auto headers : { "", "Content-Type: application/octet-stream\r\nContent-Disposition: form-data; name=file; filename=a.bin\r\n" }) {
        auto body = std::string{ headers }.empty() ? multipart_wire("1234")
            : "--b\r\n" + std::string{ headers } + "\r\n1234\r\n--b--\r\n";
        Check(mhttp::FormData::Parse(body, TYPE, { .part_bytes = 4 }).has_value(), "part exactly at its byte limit is accepted");
        CheckEq(mhttp::FormData::Parse(body, TYPE, { .part_bytes = 3 }).error(), mhttp::Error::BodyTooLarge, "both regular fields and files obey the part byte limit");
        CheckEq(mhttp::FormData::Parse(body, TYPE, { .body_bytes = body.size() - 1 }).error(), mhttp::Error::BodyTooLarge, "whole representation has an independent byte bound");
    }
    std::string many;
    for (unsigned index{}; index < 129; ++index) { many += "--b\r\nContent-Disposition: form-data; name=x\r\n\r\ny\r\n"; }
    many += "--b--\r\n";
    CheckEq(mhttp::FormData::Parse(many, TYPE).error(), mhttp::Error::BodyTooLarge, "default count bound rejects the 129th field");
    Check(mhttp::FormData::Parse(many, TYPE, { .max_parts = 129 }).has_value(), "caller can explicitly raise the part count bound");
    CheckEq(mhttp::FormData::Parse(multipart_wire(""), TYPE, { .max_parts = 0 }).error(), mhttp::Error::BodyTooLarge, "zero count admits no nonempty forms");
    Check(mhttp::FormData::Parse(multipart_wire(""), TYPE, { .part_bytes = 0 }).has_value(), "zero content bound admits an empty field");
    CheckEq(mhttp::FormData::Parse(multipart_wire("x", "X-A: a\r\nX-B: b\r\n"), TYPE,
        { .part_header_fields = 2 }).error(), mhttp::Error::InvalidHeaders, "ignored headers still count toward admission");
    CheckEq(mhttp::FormData::Parse(multipart_wire("x"), TYPE, { .part_header_bytes = 16 }).error(),
        mhttp::Error::InvalidHeaders, "header bytes are bounded before metadata allocation");
}

/**
 * @brief Parse on demand without mutating HTTP state, caching shared data, or retaining a Request owner.
 */
auto request_multipart_ownership() -> void {
    mhttp::FormData retained;
    {
        mhttp::detail::RequestHead head{ .method = "POST" };
        Check(head.headers.Set("Content-Type", "multipart/form-data; boundary=b").has_value(), "set multipart declaration");
        auto request = mhttp::detail::ServerRequestAccess::Create(std::move(head));
        auto body = multipart_wire("original");
        mhttp::detail::ServerRequestAccess::SetBody(request, body);
        auto parsed = request.Multipart();
        if (!Check(parsed.has_value(), "Request exposes multipart parsing on demand")) { return; }
        parsed->Add({ .name = "new", .content = "independent" });
        CheckEq(request.Multipart()->Size(), std::size_t{ 1 }, "each parse returns independent storage");
        CheckEq(request.Body(), body, "Multipart never rewrites original body bytes");
        retained = std::move(*parsed);
    }
    Check(retained.Get("field") && retained.Get("field")->content == "original", "form owns values after Request destruction");
    mhttp::detail::RequestHead duplicate;
    Check(duplicate.headers.Add("Content-Type", "multipart/form-data; boundary=b").has_value(), "first multipart declaration");
    Check(duplicate.headers.Add("Content-Type", "multipart/form-data; boundary=b").has_value(), "duplicate multipart declaration");
    auto request = mhttp::detail::ServerRequestAccess::Create(std::move(duplicate));
    CheckEq(request.Multipart().error(), mhttp::Error::InvalidHeaders, "duplicate Content-Type is rejected before parsing");
}

/**
 * @brief Compare missing-boundary scan cost for four and sixteen MiB inputs, outside allocation timing.
 */
auto multipart_linear_scan() -> void {
    std::string small(4 * 1024 * 1024, 'x'), large(16 * 1024 * 1024, 'x');
    auto measure = [](std::string_view input) {
        auto started = std::chrono::steady_clock::now();
        for (unsigned round{}; round < 32; ++round) {
            auto parsed = mhttp::FormData::Parse(input, "multipart/form-data; boundary=missing", { .body_bytes = 20 * 1024 * 1024 });
            Check(!parsed && parsed.error() == mhttp::Error::InvalidRequest, "scan actually rejects a missing initial boundary");
        }
        return std::chrono::steady_clock::now() - started;
    };
    std::array<std::chrono::steady_clock::duration, 3> smaller{}, larger{};
    for (std::size_t index{}; index < smaller.size(); ++index) { smaller[index] = measure(small); larger[index] = measure(large); }
    std::ranges::sort(smaller);
    std::ranges::sort(larger);
    Check(larger[1] < smaller[1] * 12, "quadrupling input stays below quadratic sixteen-fold scan cost");
}

/**
 * @brief Run multipart admission under a bounded test-process watchdog.
 * @param argc Argument count.
 * @param argv Optional filters.
 * @return Test status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "multipart boundary parameters", multipart_boundaries },
        { "multipart owned ordered parts", multipart_owned_parts },
        { "multipart rejection and limits", multipart_rejection_and_limits },
        { "Request multipart ownership", request_multipart_ownership },
        { "multipart missing boundary scan", multipart_linear_scan },
    });
}
