/**
 * @file smoke_test.cpp
 * @brief Verify public-module imports and the standalone test harness.
 */
import std;
import mhttp;

#include "support/test_support.hpp"

/**
 * @brief Exercise successful assertions from the caller and a joined worker thread.
 */
auto assertions_pass() -> void {
    mhttp::test::Check(true, "boolean expectation passes");
    mhttp::test::CheckEq(2 + 2, 4, "equality expectation passes");
    std::jthread worker{ [] { mhttp::test::Check(true, "worker expectation passes"); } };
}

/**
 * @brief Deliberately fail to verify exit status and caller diagnostics in a subprocess.
 */
auto assertion_fails() -> void {
    std::jthread worker{ [] { mhttp::test::CheckEq(1, 2, "intentional worker failure"); } };
}

/**
 * @brief Deliberately throw to verify that the runner reports and continues execution.
 */
auto exception_fails() -> void {
    throw std::runtime_error{ "intentional exception" };
}

/**
 * @brief Block until the subprocess watchdog aborts this intentionally stuck case.
 */
auto deadline_fails() -> void {
    std::promise<void> never_completed;
    never_completed.get_future().wait();
}

/**
 * @brief Run smoke cases, or intentional failures when explicitly selected.
 * @param argc Argument count.
 * @param argv Case filters or --self-test-failure for subprocess validation.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    if (argc == 2 && std::string_view{ argv[1] } == "--self-test-failure") {
        return mhttp::test::RunTests(1, argv, {
            { "assertion failure is recorded", assertion_fails },
            { "exception is caught", exception_fails },
            { "execution continues after exceptions", assertions_pass },
        });
    }
    if (argc == 2 && std::string_view{ argv[1] } == "--self-test-deadline") {
        return mhttp::test::RunTests(1, argv,
            { { "intentional timeout", deadline_fails } }, std::chrono::milliseconds{ 20 });
    }
    return mhttp::test::RunTests(argc, argv, { { "public module smoke", assertions_pass } });
}
