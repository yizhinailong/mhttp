/**
 * @file status.cppm
 * @brief Define HTTP status values, reason phrases, and numeric response classes.
 */
export module mhttp:core.status;

import std;

export namespace mhttp {

    /**
     * @brief Name HTTP status codes without restricting parsing to known values.
     * @note 104 is temporarily registered; 418 retains its conventional legacy reason phrase.
     * @see https://www.iana.org/assignments/http-status-codes/
     */
    enum class Status : std::uint16_t {
        Unknown = 0, ///< Unknown.
        Continue = 100, ///< Continue.
        SwitchingProtocols = 101, ///< Switching Protocols.
        Processing = 102, ///< Processing.
        EarlyHints = 103, ///< Early Hints.
        UploadResumptionSupported = 104, ///< Upload Resumption Supported.
        Ok = 200, ///< OK.
        Created = 201, ///< Created.
        Accepted = 202, ///< Accepted.
        NonAuthoritativeInformation = 203, ///< Non-Authoritative Information.
        NoContent = 204, ///< No Content.
        ResetContent = 205, ///< Reset Content.
        PartialContent = 206, ///< Partial Content.
        MultiStatus = 207, ///< Multi-Status.
        AlreadyReported = 208, ///< Already Reported.
        ImUsed = 226, ///< IM Used.
        MultipleChoices = 300, ///< Multiple Choices.
        MovedPermanently = 301, ///< Moved Permanently.
        Found = 302, ///< Found.
        SeeOther = 303, ///< See Other.
        NotModified = 304, ///< Not Modified.
        UseProxy = 305, ///< Use Proxy.
        Unused = 306, ///< Unused.
        TemporaryRedirect = 307, ///< Temporary Redirect.
        PermanentRedirect = 308, ///< Permanent Redirect.
        BadRequest = 400, ///< Bad Request.
        Unauthorized = 401, ///< Unauthorized.
        PaymentRequired = 402, ///< Payment Required.
        Forbidden = 403, ///< Forbidden.
        NotFound = 404, ///< Not Found.
        MethodNotAllowed = 405, ///< Method Not Allowed.
        NotAcceptable = 406, ///< Not Acceptable.
        ProxyAuthenticationRequired = 407, ///< Proxy Authentication Required.
        RequestTimeout = 408, ///< Request Timeout.
        Conflict = 409, ///< Conflict.
        Gone = 410, ///< Gone.
        LengthRequired = 411, ///< Length Required.
        PreconditionFailed = 412, ///< Precondition Failed.
        ContentTooLarge = 413, ///< Content Too Large.
        UriTooLong = 414, ///< URI Too Long.
        UnsupportedMediaType = 415, ///< Unsupported Media Type.
        RangeNotSatisfiable = 416, ///< Range Not Satisfiable.
        ExpectationFailed = 417, ///< Expectation Failed.
        ImATeapot = 418, ///< I'm a teapot.
        MisdirectedRequest = 421, ///< Misdirected Request.
        UnprocessableContent = 422, ///< Unprocessable Content.
        Locked = 423, ///< Locked.
        FailedDependency = 424, ///< Failed Dependency.
        TooEarly = 425, ///< Too Early.
        UpgradeRequired = 426, ///< Upgrade Required.
        PreconditionRequired = 428, ///< Precondition Required.
        TooManyRequests = 429, ///< Too Many Requests.
        RequestHeaderFieldsTooLarge = 431, ///< Request Header Fields Too Large.
        UnavailableForLegalReasons = 451, ///< Unavailable For Legal Reasons.
        InternalServerError = 500, ///< Internal Server Error.
        NotImplemented = 501, ///< Not Implemented.
        BadGateway = 502, ///< Bad Gateway.
        ServiceUnavailable = 503, ///< Service Unavailable.
        GatewayTimeout = 504, ///< Gateway Timeout.
        HttpVersionNotSupported = 505, ///< HTTP Version Not Supported.
        VariantAlsoNegotiates = 506, ///< Variant Also Negotiates.
        InsufficientStorage = 507, ///< Insufficient Storage.
        LoopDetected = 508, ///< Loop Detected.
        NotExtended = 510, ///< Not Extended.
        NetworkAuthenticationRequired = 511, ///< Network Authentication Required.
    };

    /**
     * @brief Obtain the conventional reason phrase for a known status.
     * @param status Known or extension status value.
     * @return Static phrase, or Unknown for a status with no named phrase.
     */
    [[nodiscard]] constexpr auto ReasonPhrase(Status status) noexcept -> std::string_view {
        switch (status) {
        case Status::Unknown: return "Unknown";
        case Status::Continue: return "Continue";
        case Status::SwitchingProtocols: return "Switching Protocols";
        case Status::Processing: return "Processing";
        case Status::EarlyHints: return "Early Hints";
        case Status::UploadResumptionSupported: return "Upload Resumption Supported";
        case Status::Ok: return "OK";
        case Status::Created: return "Created";
        case Status::Accepted: return "Accepted";
        case Status::NonAuthoritativeInformation: return "Non-Authoritative Information";
        case Status::NoContent: return "No Content";
        case Status::ResetContent: return "Reset Content";
        case Status::PartialContent: return "Partial Content";
        case Status::MultiStatus: return "Multi-Status";
        case Status::AlreadyReported: return "Already Reported";
        case Status::ImUsed: return "IM Used";
        case Status::MultipleChoices: return "Multiple Choices";
        case Status::MovedPermanently: return "Moved Permanently";
        case Status::Found: return "Found";
        case Status::SeeOther: return "See Other";
        case Status::NotModified: return "Not Modified";
        case Status::UseProxy: return "Use Proxy";
        case Status::Unused: return "Unused";
        case Status::TemporaryRedirect: return "Temporary Redirect";
        case Status::PermanentRedirect: return "Permanent Redirect";
        case Status::BadRequest: return "Bad Request";
        case Status::Unauthorized: return "Unauthorized";
        case Status::PaymentRequired: return "Payment Required";
        case Status::Forbidden: return "Forbidden";
        case Status::NotFound: return "Not Found";
        case Status::MethodNotAllowed: return "Method Not Allowed";
        case Status::NotAcceptable: return "Not Acceptable";
        case Status::ProxyAuthenticationRequired: return "Proxy Authentication Required";
        case Status::RequestTimeout: return "Request Timeout";
        case Status::Conflict: return "Conflict";
        case Status::Gone: return "Gone";
        case Status::LengthRequired: return "Length Required";
        case Status::PreconditionFailed: return "Precondition Failed";
        case Status::ContentTooLarge: return "Content Too Large";
        case Status::UriTooLong: return "URI Too Long";
        case Status::UnsupportedMediaType: return "Unsupported Media Type";
        case Status::RangeNotSatisfiable: return "Range Not Satisfiable";
        case Status::ExpectationFailed: return "Expectation Failed";
        case Status::ImATeapot: return "I'm a teapot";
        case Status::MisdirectedRequest: return "Misdirected Request";
        case Status::UnprocessableContent: return "Unprocessable Content";
        case Status::Locked: return "Locked";
        case Status::FailedDependency: return "Failed Dependency";
        case Status::TooEarly: return "Too Early";
        case Status::UpgradeRequired: return "Upgrade Required";
        case Status::PreconditionRequired: return "Precondition Required";
        case Status::TooManyRequests: return "Too Many Requests";
        case Status::RequestHeaderFieldsTooLarge: return "Request Header Fields Too Large";
        case Status::UnavailableForLegalReasons: return "Unavailable For Legal Reasons";
        case Status::InternalServerError: return "Internal Server Error";
        case Status::NotImplemented: return "Not Implemented";
        case Status::BadGateway: return "Bad Gateway";
        case Status::ServiceUnavailable: return "Service Unavailable";
        case Status::GatewayTimeout: return "Gateway Timeout";
        case Status::HttpVersionNotSupported: return "HTTP Version Not Supported";
        case Status::VariantAlsoNegotiates: return "Variant Also Negotiates";
        case Status::InsufficientStorage: return "Insufficient Storage";
        case Status::LoopDetected: return "Loop Detected";
        case Status::NotExtended: return "Not Extended";
        case Status::NetworkAuthenticationRequired: return "Network Authentication Required";
        }
        return "Unknown";
    }

    /**
     * @brief Recognize the informational numeric status class.
     * @param status Known or extension status.
     * @return Whether the numeric value lies in [100, 200).
     */
    [[nodiscard]] constexpr auto IsInformational(Status status) noexcept -> bool {
        auto const value = static_cast<unsigned>(status);
        return value >= 100 && value < 200;
    }

    /**
     * @brief Recognize the successful numeric status class.
     * @param status Known or extension status.
     * @return Whether the numeric value lies in [200, 300).
     */
    [[nodiscard]] constexpr auto IsSuccess(Status status) noexcept -> bool {
        auto const value = static_cast<unsigned>(status);
        return value >= 200 && value < 300;
    }

    /**
     * @brief Recognize the redirection numeric status class.
     * @param status Known or extension status.
     * @return Whether the numeric value lies in [300, 400).
     */
    [[nodiscard]] constexpr auto IsRedirect(Status status) noexcept -> bool {
        auto const value = static_cast<unsigned>(status);
        return value >= 300 && value < 400;
    }

    /**
     * @brief Recognize the client-error numeric status class.
     * @param status Known or extension status.
     * @return Whether the numeric value lies in [400, 500).
     */
    [[nodiscard]] constexpr auto IsClientError(Status status) noexcept -> bool {
        auto const value = static_cast<unsigned>(status);
        return value >= 400 && value < 500;
    }

    /**
     * @brief Recognize the server-error numeric status class.
     * @param status Known or extension status.
     * @return Whether the numeric value lies in [500, 600).
     */
    [[nodiscard]] constexpr auto IsServerError(Status status) noexcept -> bool {
        auto const value = static_cast<unsigned>(status);
        return value >= 500 && value < 600;
    }

} // namespace mhttp

