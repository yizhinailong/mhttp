/**
 * @file error.cppm
 * @brief Define transport-independent errors, expected results, and standard error-code integration.
 */
export module mhttp:core.error;

import std;

export namespace mhttp {

    /**
     * @brief Identify recoverable HTTP, transport, and lifecycle failures.
     * @note All enumerators are nonzero so every error converts to a truthy std::error_code.
     */
    enum class Error {
        Unknown = 1, ///< An unclassified operation failure.
        Connection, ///< Connection establishment failed.
        ConnectionTimeout, ///< Connection deadline expired.
        ResolveFailed, ///< Hostname resolution failed.
        BindIpAddress, ///< Local address binding failed.
        Read, ///< Reading failed.
        ReadTimeout, ///< Read deadline expired.
        Write, ///< Writing failed.
        WriteTimeout, ///< Write deadline expired.
        ConnectionClosed, ///< Peer closed the connection before the operation completed.
        Canceled, ///< Operation was canceled.
        ExceedRedirectCount, ///< Redirect limit was exceeded.
        InvalidUrl, ///< URL syntax is invalid.
        UnsupportedScheme, ///< URL scheme is not supported by this transport.
        InvalidHeaders, ///< Header names or values are invalid.
        InvalidRequest, ///< Request syntax or configuration is invalid.
        ProtocolError, ///< Peer violated HTTP framing or syntax.
        HeaderTooLarge, ///< Header size or field count exceeded its limit.
        BodyTooLarge, ///< Body size exceeded its limit.
        UnsupportedContentEncoding, ///< Response requires an unsupported content decoder.
        Busy, ///< Another operation is already in flight.
        AlreadyRunning, ///< Component is already running.
        NotRunning, ///< Operation requires a running component.
        AcquireTimeout, ///< No pool connection became available within the acquisition deadline.
    };

    /**
     * @brief Represent either an owned success value or a recoverable failure.
     * @tparam T Success type; void represents successful completion without a value.
     */
    template <typename T>
    using Result = std::expected<T, Error>;

    /**
     * @brief Describe an error without allocating or exposing backend diagnostics.
     * @param error Error value to describe.
     * @return A static string, including a fallback for values outside the enumeration.
     */
    [[nodiscard]] constexpr auto ToString(Error error) noexcept -> std::string_view {
        switch (error) {
        case Error::Unknown: return "Unknown operation failure";
        case Error::Connection: return "Connection failed";
        case Error::ConnectionTimeout: return "Connection timed out";
        case Error::ResolveFailed: return "Hostname resolution failed";
        case Error::BindIpAddress: return "Local address binding failed";
        case Error::Read: return "Read failed";
        case Error::ReadTimeout: return "Read timed out";
        case Error::Write: return "Write failed";
        case Error::WriteTimeout: return "Write timed out";
        case Error::ConnectionClosed: return "Connection closed prematurely";
        case Error::Canceled: return "Operation canceled";
        case Error::ExceedRedirectCount: return "Redirect limit exceeded";
        case Error::InvalidUrl: return "Invalid URL";
        case Error::UnsupportedScheme: return "Unsupported URL scheme";
        case Error::InvalidHeaders: return "Invalid HTTP headers";
        case Error::InvalidRequest: return "Invalid HTTP request";
        case Error::ProtocolError: return "HTTP protocol error";
        case Error::HeaderTooLarge: return "HTTP header limit exceeded";
        case Error::BodyTooLarge: return "HTTP body limit exceeded";
        case Error::UnsupportedContentEncoding: return "Unsupported content encoding";
        case Error::Busy: return "Operation already in flight";
        case Error::AlreadyRunning: return "Component already running";
        case Error::NotRunning: return "Component not running";
        case Error::AcquireTimeout: return "Pool acquisition timed out";
        }
        return "Unrecognized mhttp error";
    }

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Supply a stable category for standard error-code interoperability.
     * @internal
     */
    class HttpErrorCategory final : public std::error_category {
    public:
        /**
         * @brief Return the stable category identifier.
         * @return Static category name.
         */
        auto name() const noexcept -> char const* override {
            return "mhttp";
        }

        /**
         * @brief Describe a numeric error value.
         * @param value Numeric representation of Error.
         * @return Owned error description.
         */
        auto message(int value) const -> std::string override {
            return std::string{ ToString(static_cast<Error>(value)) };
        }
    };

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Convert an error for interoperability with standard error facilities.
     * @param error Recoverable mhttp error.
     * @return Error code referring to the process-wide mhttp category.
     * @note This spelling is required for argument-dependent lookup by std::error_code.
     */
    [[nodiscard]] auto make_error_code(Error error) noexcept -> std::error_code {
        static detail::HttpErrorCategory const s_category;
        return { static_cast<int>(error), s_category };
    }

} // namespace mhttp

export namespace std {

    /**
     * @brief Enable implicit conversion of mhttp errors to standard error codes.
     */
    template <>
    struct is_error_code_enum<mhttp::Error> : true_type {};

} // namespace std
