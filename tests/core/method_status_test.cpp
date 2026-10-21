/**
 * @file method_status_test.cpp
 * @brief Verify HTTP method spellings, status identities, phrases, and numeric boundaries.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;

/**
 * @brief Verify supported HTTP and extension method names using their wire spellings.
 */
auto method_names() -> void {
    using enum mhttp::Method;
    constexpr std::pair<mhttp::Method, std::string_view> METHODS[]{
        { Get, "GET" }, { Head, "HEAD" }, { Post, "POST" }, { Put, "PUT" }, { Delete, "DELETE" },
        { Connect, "CONNECT" }, { Options, "OPTIONS" }, { Trace, "TRACE" }, { Patch, "PATCH" },
        { Copy, "COPY" }, { Lock, "LOCK" }, { MkCol, "MKCOL" }, { Move, "MOVE" },
        { PropFind, "PROPFIND" }, { PropPatch, "PROPPATCH" }, { Search, "SEARCH" }, { Unlock, "UNLOCK" },
        { Bind, "BIND" }, { Rebind, "REBIND" }, { Unbind, "UNBIND" }, { Acl, "ACL" },
        { Report, "REPORT" }, { MkActivity, "MKACTIVITY" }, { Checkout, "CHECKOUT" }, { Merge, "MERGE" },
        { MSearch, "M-SEARCH" }, { Notify, "NOTIFY" }, { Subscribe, "SUBSCRIBE" },
        { Unsubscribe, "UNSUBSCRIBE" }, { Purge, "PURGE" }, { MkCalendar, "MKCALENDAR" },
        { Link, "LINK" }, { Unlink, "UNLINK" },
    };
    for (auto const& [method, text] : METHODS) {
        CheckEq(mhttp::ToString(method), text, std::format("wire spelling of {}", text));
        CheckEq(mhttp::ParseMethod(text), std::optional{ method }, std::format("parse {}", text));
        Check(!mhttp::ParseMethod(mhttp::utils::ToLower(text)), "method spelling is case-sensitive");
    }
    for (auto text : { "", "BREW", "Get", " GET", "GET ", "GET\r\n", "MSEARCH" }) {
        Check(!mhttp::ParseMethod(text), std::format("unknown method {:?}", text));
    }
    Check(!mhttp::ParseMethod(std::string_view{ "GET\0", 4 }), "method parsing preserves input length");
    Check(mhttp::ToString(static_cast<mhttp::Method>(9999)).empty(), "invalid method has no wire name");
    Check(mhttp::ToString(static_cast<mhttp::Method>(-1)).empty(), "negative method has no wire name");
}

/**
 * @brief Verify named statuses against explicit protocol numbers and reason phrases.
 */
auto status_names() -> void {
    using enum mhttp::Status;
    struct Case {
        mhttp::Status status;
        unsigned number;
        std::string_view phrase;
    };
    constexpr Case CASES[]{
        { Unknown, 0, "Unknown" }, { Continue, 100, "Continue" },
        { SwitchingProtocols, 101, "Switching Protocols" }, { Processing, 102, "Processing" },
        { EarlyHints, 103, "Early Hints" }, { UploadResumptionSupported, 104, "Upload Resumption Supported" },
        { Ok, 200, "OK" }, { Created, 201, "Created" }, { Accepted, 202, "Accepted" },
        { NonAuthoritativeInformation, 203, "Non-Authoritative Information" },
        { NoContent, 204, "No Content" }, { ResetContent, 205, "Reset Content" },
        { PartialContent, 206, "Partial Content" }, { MultiStatus, 207, "Multi-Status" },
        { AlreadyReported, 208, "Already Reported" }, { ImUsed, 226, "IM Used" },
        { MultipleChoices, 300, "Multiple Choices" }, { MovedPermanently, 301, "Moved Permanently" },
        { Found, 302, "Found" }, { SeeOther, 303, "See Other" }, { NotModified, 304, "Not Modified" },
        { UseProxy, 305, "Use Proxy" }, { Unused, 306, "Unused" },
        { TemporaryRedirect, 307, "Temporary Redirect" }, { PermanentRedirect, 308, "Permanent Redirect" },
        { BadRequest, 400, "Bad Request" }, { Unauthorized, 401, "Unauthorized" },
        { PaymentRequired, 402, "Payment Required" }, { Forbidden, 403, "Forbidden" },
        { NotFound, 404, "Not Found" }, { MethodNotAllowed, 405, "Method Not Allowed" },
        { NotAcceptable, 406, "Not Acceptable" }, { ProxyAuthenticationRequired, 407, "Proxy Authentication Required" },
        { RequestTimeout, 408, "Request Timeout" }, { Conflict, 409, "Conflict" }, { Gone, 410, "Gone" },
        { LengthRequired, 411, "Length Required" }, { PreconditionFailed, 412, "Precondition Failed" },
        { ContentTooLarge, 413, "Content Too Large" }, { UriTooLong, 414, "URI Too Long" },
        { UnsupportedMediaType, 415, "Unsupported Media Type" }, { RangeNotSatisfiable, 416, "Range Not Satisfiable" },
        { ExpectationFailed, 417, "Expectation Failed" }, { ImATeapot, 418, "I'm a teapot" },
        { MisdirectedRequest, 421, "Misdirected Request" }, { UnprocessableContent, 422, "Unprocessable Content" },
        { Locked, 423, "Locked" }, { FailedDependency, 424, "Failed Dependency" }, { TooEarly, 425, "Too Early" },
        { UpgradeRequired, 426, "Upgrade Required" }, { PreconditionRequired, 428, "Precondition Required" },
        { TooManyRequests, 429, "Too Many Requests" }, { RequestHeaderFieldsTooLarge, 431, "Request Header Fields Too Large" },
        { UnavailableForLegalReasons, 451, "Unavailable For Legal Reasons" },
        { InternalServerError, 500, "Internal Server Error" }, { NotImplemented, 501, "Not Implemented" },
        { BadGateway, 502, "Bad Gateway" }, { ServiceUnavailable, 503, "Service Unavailable" },
        { GatewayTimeout, 504, "Gateway Timeout" }, { HttpVersionNotSupported, 505, "HTTP Version Not Supported" },
        { VariantAlsoNegotiates, 506, "Variant Also Negotiates" }, { InsufficientStorage, 507, "Insufficient Storage" },
        { LoopDetected, 508, "Loop Detected" }, { NotExtended, 510, "Not Extended" },
        { NetworkAuthenticationRequired, 511, "Network Authentication Required" },
    };
    for (auto const& test : CASES) {
        CheckEq(static_cast<unsigned>(test.status), test.number, std::format("status {} numeric identity", test.phrase));
        CheckEq(mhttp::ReasonPhrase(test.status), test.phrase, std::format("reason for {}", test.number));
    }
    CheckEq(mhttp::ReasonPhrase(static_cast<mhttp::Status>(9999)), "Unknown", "unknown positive status");
    CheckEq(mhttp::ReasonPhrase(static_cast<mhttp::Status>(-1)), "Unknown", "unknown negative status");
}

/**
 * @brief Verify range endpoints and extensions across the entire status-code space.
 */
auto status_classes() -> void {
    for (unsigned number{}; number <= 1000; ++number) {
        auto const status = static_cast<mhttp::Status>(number);
        auto const message = std::format("numeric class of {}", number);
        CheckEq(mhttp::IsInformational(status), number / 100 == 1, message);
        CheckEq(mhttp::IsSuccess(status), number / 100 == 2, message);
        CheckEq(mhttp::IsRedirect(status), number / 100 == 3, message);
        CheckEq(mhttp::IsClientError(status), number / 100 == 4, message);
        CheckEq(mhttp::IsServerError(status), number / 100 == 5, message);
    }
}

/**
 * @brief Run method and status cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "HTTP method names", method_names },
        { "HTTP status names", status_names },
        { "HTTP status classes", status_classes },
    });
}
