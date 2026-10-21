/**
 * @file error_test.cpp
 * @brief Verify error descriptions, implicit standard conversions, results, and owned endpoints.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using mhttp::test::CheckEq;

/**
 * @brief Verify every declared error has a distinct description and standard error-code value.
 */
auto error_codes() -> void {
    using enum mhttp::Error;
    constexpr mhttp::Error ERRORS[]{
        Unknown, Connection, ConnectionTimeout, ResolveFailed, BindIpAddress, Read, ReadTimeout,
        Write, WriteTimeout, ConnectionClosed, Canceled, ExceedRedirectCount, InvalidUrl,
        UnsupportedScheme, InvalidHeaders, InvalidRequest, ProtocolError, HeaderTooLarge,
        BodyTooLarge, UnsupportedContentEncoding, Busy, AlreadyRunning, NotRunning,
    };
    std::unordered_set<std::string_view> descriptions;
    auto const fallback = mhttp::ToString(static_cast<mhttp::Error>(9999));
    for (auto error : ERRORS) {
        std::error_code const code = error;
        Check(static_cast<bool>(code), "every error code represents failure");
        CheckEq(code.category().name(), std::string_view{ "mhttp" }, "stable category name");
        CheckEq(code.value(), static_cast<int>(error), "numeric error identity");
        CheckEq(code.message(), mhttp::ToString(error), "standard message matches public description");
        CheckEq(code, mhttp::make_error_code(error), "implicit conversion uses the same category");
        Check(!mhttp::ToString(error).empty(), "error description is nonempty");
        Check(mhttp::ToString(error) != fallback, "known errors do not use fallback text");
        Check(descriptions.insert(mhttp::ToString(error)).second, "error descriptions are distinct");
    }
    std::error_code const canceled = Canceled;
    Check(canceled == Canceled, "standard comparison supports Error values");
    Check(canceled != Busy, "different errors compare unequal");
    CheckEq(mhttp::make_error_code(static_cast<mhttp::Error>(-1)).message(), fallback,
        "out-of-range negative errors use the fallback");
}

/**
 * @brief Verify value/error results, void completion, and move-only success values.
 */
auto expected_results() -> void {
    mhttp::Result<int> value{ 42 };
    CheckEq(value.value(), 42, "result carries its value");
    mhttp::Result<int> failure{ std::unexpected{ mhttp::Error::Canceled } };
    Check(!failure, "result carries failure");
    CheckEq(failure.error(), mhttp::Error::Canceled, "result retains the original error");
    mhttp::Result<void> complete{};
    Check(complete.has_value(), "void completion succeeds");
    mhttp::Result<std::unique_ptr<int>> owned{ std::make_unique<int>(7) };
    auto moved = std::move(owned);
    CheckEq(**moved, 7, "result owns move-only values");
}

/**
 * @brief Verify mhttp errors survive standard exception construction, propagation, and destruction.
 */
auto system_errors() -> void {
    for (auto error : { mhttp::Error::Busy, mhttp::Error::NotRunning, mhttp::Error::Canceled }) {
        bool caught{};
        try { throw std::system_error{ mhttp::make_error_code(error) }; }
        catch (std::system_error const& exception) {
            caught = exception.code() == error;
            Check(!std::string_view{ exception.what() }.empty(), "system error has a diagnostic");
        }
        Check(caught, "system error preserves the mhttp category and value");
        std::exception_ptr captured;
        try { throw std::system_error{ mhttp::make_error_code(error) }; }
        catch (...) { captured = std::current_exception(); }
        caught = false;
        try { std::rethrow_exception(captured); }
        catch (std::system_error const& exception) { caught = exception.code() == error; }
        Check(caught, "captured system error preserves category and value");
        captured = std::make_exception_ptr(std::system_error{ mhttp::make_error_code(error) });
        caught = false;
        try { std::rethrow_exception(captured); }
        catch (std::system_error const& exception) { caught = exception.code() == error; }
        Check(caught, "copied system error preserves category and value");
    }
}

/**
 * @brief Verify endpoint storage is owned and independent of source strings and copies.
 */
auto endpoint_values() -> void {
    std::string address{ "::1" };
    mhttp::Endpoint endpoint{ address, 8080 };
    address.clear();
    CheckEq(endpoint.address, "::1", "endpoint owns its address");
    auto copy = endpoint;
    copy.port = 80;
    Check(endpoint != copy, "copies are independent value objects");
    CheckEq(endpoint, mhttp::Endpoint{ "::1", 8080 }, "endpoint equality compares values");
    CheckEq(mhttp::Endpoint{}.port, std::uint16_t{}, "default port requests dynamic binding");
}

/**
 * @brief Run error, result, and endpoint cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "standard error codes", error_codes },
        { "expected results", expected_results },
        { "standard system errors", system_errors },
        { "endpoint values", endpoint_values },
    });
}
