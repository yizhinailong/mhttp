/**
 * @file error.cppm
 * @brief Provide mhttp's error codes for body handling failures.
 */
module;

#include <boost/system/error_category.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:error;

import std;

export namespace mhttp {

    /**
     * @brief Failures reported by mhttp itself (network and parser errors keep their own codes).
     */
    enum class Error {
        InvalidJson = 1,     ///< A JSON body or document did not parse.
        BodyTypeMismatch,    ///< The body was materialized as a different type.
        BodyAlreadyStreamed, ///< The body was consumed by streaming reads and cannot be materialized.
        OperationInProgress, ///< Another read or write on the same message has not finished.
    };

    /**
     * @brief Return the category of mhttp::Error codes.
     * @return Process-wide category named "mhttp".
     */
    auto error_category() noexcept -> boost::system::error_category const&;

    /**
     * @brief Wrap an mhttp::Error into an error code.
     * @param error Error value.
     * @return Error code in the mhttp category.
     */
    auto make_error_code(Error error) noexcept -> boost::system::error_code;

} // namespace mhttp

namespace mhttp {

    /**
     * @brief Category describing mhttp::Error values.
     */
    class ErrorCategory : public boost::system::error_category {
    public:
        auto name() const noexcept -> char const* override {
            return "mhttp";
        }

        auto message(int value) const -> std::string override {
            switch (static_cast<Error>(value)) {
            case Error::InvalidJson:
                return "body is not valid JSON";
            case Error::BodyTypeMismatch:
                return "body was materialized as a different type";
            case Error::BodyAlreadyStreamed:
                return "body was already consumed by streaming reads";
            case Error::OperationInProgress:
                return "another operation on this message is in progress";
            }
            return "unknown mhttp error";
        }
    };

    auto error_category() noexcept -> boost::system::error_category const& {
        static ErrorCategory const s_category;
        return s_category;
    }

    auto make_error_code(Error error) noexcept -> boost::system::error_code {
        return boost::system::error_code{ static_cast<int>(error), error_category() };
    }

} // namespace mhttp

// Lets `ec = mhttp::Error::X` and `ec == mhttp::Error::X` work through ADL on make_error_code.
template <>
struct boost::system::is_error_code_enum<mhttp::Error> : std::true_type {};
