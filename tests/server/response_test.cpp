/**
 * @file response_test.cpp
 * @brief Verify buffered server responses, rejected mutations, redirect ownership, and protocol framing.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;
using namespace std::string_view_literals;

/**
 * @brief Set binary content and arbitrary valid statuses while preserving unrelated ordered fields.
 */
auto content_and_status() -> void {
    mhttp::server::Response response;
    CheckEq(response.Status(), mhttp::Status::Ok, "default status");
    Check(response.Body().empty() && response.Headers().Empty(), "default has no representation or inferred type");
    Check(response.Headers().Add("X-Trace", "one").has_value(), "first field");
    Check(response.Headers().Add("X-Trace", "two").has_value(), "repeated field");
    Check(response.SetStatus(mhttp::Status::Created).has_value(), "set created status");
    Check(response.SetContent(std::string{ "a\0b"sv }, "application/octet-stream").has_value(), "binary content");
    CheckEq(response.Body(), "a\0b"sv, "binary representation owns all bytes");
    CheckEq(response.Header("content-type"), "application/octet-stream"sv, "type lookup ignores case");
    CheckEq(response.Status(), mhttp::Status::Created, "SetContent preserves status");
    CheckEq(response.Headers().Values("X-Trace"), std::vector{ "one"sv, "two"sv }, "unrelated fields preserved");
    auto const& view = std::as_const(response);
    CheckEq(view.Headers().Count("X-Trace"), std::size_t{ 2 }, "const field access");
    CheckEq(view.Header("missing", "fallback"), "fallback"sv, "header fallback");
    Check(response.SetContent(response.Body(), response.Header("Content-Type")).has_value(), "content accepts self-borrowed inputs");
    CheckEq(response.Body(), "a\0b"sv, "self-borrowed content retained");
    Check(response.SetContent("untyped", "").has_value(), "explicit untyped content");
    Check(!response.Headers().Has("Content-Type"), "empty content type removes field");
    Check(response.SetStatus(static_cast<mhttp::Status>(299)).has_value(), "extension status accepted");
    Check(response.SetEmpty(mhttp::Status::NoContent).has_value(), "empty representation");
    Check(response.Body().empty() && !response.Headers().Has("Content-Type"), "empty clears representation metadata");
    Check(response.Headers().Has("X-Trace"), "empty preserves unrelated fields");
    auto moved = std::move(response);
    CheckEq(moved.Status(), mhttp::Status::NoContent, "move transfers response ownership");
    static_assert(!std::is_copy_constructible_v<mhttp::server::Response>);
    static_assert(std::is_nothrow_move_constructible_v<mhttp::server::Response>);
}

/**
 * @brief Reject invalid statuses and header injection while preserving the entire prior response.
 */
auto rejected_mutations() -> void {
    mhttp::server::Response response;
    Check(response.SetContent("original", "text/plain").has_value(), "initial response");
    Check(response.SetStatus(mhttp::Status::Accepted).has_value(), "initial status");
    for (auto status : { mhttp::Status::Unknown, static_cast<mhttp::Status>(99), static_cast<mhttp::Status>(1000) }) {
        Check(!response.SetStatus(status) && !response.SetEmpty(status), "reject non-three-digit status");
    }
    Check(!response.SetError(mhttp::Status::Ok), "SetError rejects a non-error status");
    for (auto type : { "text/plain\r\nX-Evil: yes"sv, "a\0b"sv, " leading"sv }) {
        auto result = response.SetContent("changed", type);
        Check(!result && result.error() == mhttp::Error::InvalidHeaders, "reject unsafe content type");
    }
    for (auto url : { "/ok\r\nX-Evil: yes"sv, "/a\0b"sv }) {
        auto result = response.SetRedirect(url);
        Check(!result && result.error() == mhttp::Error::InvalidHeaders, "reject redirect splitting");
    }
    Check(!response.SetRedirect("/new", mhttp::Status::Ok), "redirect rejects non-3xx status");
    Check(!response.SetRedirect("/new", mhttp::Status::NotModified), "304 is not a redirect representation");
    Check(!response.SetRedirect(""), "empty redirect rejected");
    CheckEq(response.Status(), mhttp::Status::Accepted, "rejected mutations preserve status");
    CheckEq(response.Body(), "original"sv, "rejected mutations preserve body");
    CheckEq(response.Header("Content-Type"), "text/plain"sv, "rejected mutations preserve type");
    Check(!response.Headers().Has("Location"), "rejected redirect adds no field");
}

/**
 * @brief Build static error text and own redirect references before borrowed source storage changes.
 */
auto errors_and_redirects() -> void {
    mhttp::server::Response response;
    Check(response.SetError(mhttp::Status::NotFound).has_value(), "default 404 error");
    CheckEq(response.Body(), "404 Not Found\n"sv, "default plain-text error body");
    CheckEq(response.Header("Content-Type"), "text/plain; charset=utf-8"sv, "error content type");
    Check(response.SetError(static_cast<mhttp::Status>(499)).has_value(), "extension error status");
    CheckEq(response.Body(), "499 Unknown\n"sv, "extension error uses the shared static reason fallback");
    std::string reference{ "/new?x=one%20two#fragment" };
    Check(response.SetRedirect(reference).has_value(), "relative redirect");
    reference.clear();
    CheckEq(response.Header("location"), "/new?x=one%20two#fragment"sv, "redirect owns unmodified reference");
    CheckEq(response.Status(), mhttp::Status::Found, "redirect defaults to 302");
    Check(response.Body().empty() && !response.Headers().Has("Content-Type"), "redirect clears former representation");
    Check(response.SetRedirect(response.Header("Location"), mhttp::Status::TemporaryRedirect).has_value(), "self-borrowed location");
    CheckEq(response.Status(), mhttp::Status::TemporaryRedirect, "explicit 307 redirect");
    Check(response.SetContent("https://example.invalid/new", "text/plain").has_value(), "body-backed location source");
    Check(response.SetRedirect(response.Body()).has_value(), "redirect copies source before clearing body");
    CheckEq(response.Header("Location"), "https://example.invalid/new"sv, "body source remains valid through copy");
}

/**
 * @brief Recompute Content-Length from actual body and suppress wire payloads for HEAD and bodyless statuses.
 */
auto response_framing() -> void {
    mhttp::server::Response response;
    Check(response.SetContent("hello", "text/plain").has_value(), "representation");
    Check(response.Headers().Set("Content-Length", "999").has_value(), "stale handler length");
    Check(response.Headers().Set("Transfer-Encoding", "gzip").has_value(), "stale handler coding");
    auto head = mhttp::detail::ServerResponseAccess::Head(response);
    auto encoded = mhttp::detail::serialize_response_head(head, mhttp::Method::Get, { .body_bytes = response.Body().size() });
    if (!Check(encoded.has_value(), "response serializes")) { return; }
    Check(encoded->send_body && encoded->bytes.contains("Content-Length: 5\r\n"), "actual representation determines length");
    Check(!encoded->bytes.contains("Transfer-Encoding"), "handler framing is replaced");
    auto for_head = mhttp::detail::serialize_response_head(head, mhttp::Method::Head, { .body_bytes = response.Body().size() });
    Check(for_head && !for_head->send_body && for_head->bytes == encoded->bytes, "HEAD has GET headers without wire body");
    Check(response.SetContent("later", "text/html").has_value(), "later response mutation");
    CheckEq(head.headers.Get("Content-Type"), "text/plain"sv, "protocol snapshot owns fields independently");
    for (auto status : { mhttp::Status::Continue, mhttp::Status::NoContent, mhttp::Status::NotModified }) {
        Check(response.SetStatus(status).has_value(), "bodyless status");
        auto suppressed = mhttp::detail::serialize_response_head(mhttp::detail::ServerResponseAccess::Head(response),
            mhttp::Method::Get, { .body_bytes = response.Body().size() });
        Check(suppressed && !suppressed->send_body && !suppressed->bytes.contains("Content-Length"), "status suppresses wire body and length");
    }
}

/**
 * @brief Run bounded response scenarios.
 * @param argc Argument count.
 * @param argv Borrowed arguments.
 * @return Nonzero on failure.
 */
auto main(int argc, char** argv) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "response content and status", content_and_status },
        { "response rejected mutations", rejected_mutations },
        { "response errors and redirects", errors_and_redirects },
        { "response protocol framing", response_framing },
    });
}
