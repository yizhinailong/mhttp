/**
 * @file error_test.cpp
 * @brief Verify reserved-domain resolution failure and an explicitly enabled connection-timeout fixture.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::Check;
using namespace std::chrono_literals;

/**
 * @brief Resolve a reserved invalid domain without contacting any HTTP service and bound completion.
 */
auto invalid_domain() -> void {
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start reserved-domain runtime");
    mhttp::client::HttpClient client{ runtime, "mhttp-client.invalid", 80, { .connect_timeout = 2s } };
    auto const started = std::chrono::steady_clock::now();
    auto response = mhttp::SyncWait(runtime, client.Get());
    Check(!response && response.error() == mhttp::Error::ResolveFailed && !client.IsOpen(), "reserved invalid domain returns ResolveFailed before HTTP exchange");
    Check(std::chrono::steady_clock::now() - started < 3s, "reserved-domain failure respects the connection budget");
}

/**
 * @brief Test a controlled endpoint configured to silently drop SYN, only when explicitly supplied by the environment.
 * @note MHTTP_TEST_CONNECT_TIMEOUT_URL must identify an HTTP URL managed by the test environment.
 */
auto opt_in_connection_timeout() -> void {
    auto const* configured = std::getenv("MHTTP_TEST_CONNECT_TIMEOUT_URL");
    if (!configured || std::string_view{ configured }.empty()) {
        std::cout << "[skip] connection timeout: set MHTTP_TEST_CONNECT_TIMEOUT_URL to a controlled SYN-drop endpoint\n";
        return;
    }
    mhttp::Runtime runtime;
    Check(runtime.Start(2).has_value(), "start controlled connection-timeout runtime");
    mhttp::client::HttpClient client{ runtime, configured, { .connect_timeout = 200ms } };
    Check(!client.InitializationError(), "controlled timeout URL is valid HTTP");
    auto const started = std::chrono::steady_clock::now();
    auto response = mhttp::SyncWait(runtime, client.Get());
    auto const elapsed = std::chrono::steady_clock::now() - started;
    Check(!response && response.error() == mhttp::Error::ConnectionTimeout && !client.IsOpen(), "silent endpoint reaches connection deadline");
    Check(elapsed >= 180ms && elapsed < 1s, "controlled SYN-drop timeout uses configured budget");
}

/**
 * @brief Run client establishment errors with explicit handling of environment-dependent fixtures.
 * @param argc Argument count.
 * @param argv Optional test filters.
 * @return Test process status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "client reserved invalid domain", invalid_domain },
        { "client opt-in connection timeout", opt_in_connection_timeout },
    });
}
