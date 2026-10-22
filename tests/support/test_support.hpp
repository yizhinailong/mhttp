/**
 * @file test_support.hpp
 * @brief Provide assertions and bounded, filterable standalone test execution.
 * @note Include after importing std and mhttp; this header imports no library headers.
 */
#pragma once

namespace mhttp::test {

    inline std::atomic<std::size_t> g_failures{}; ///< Assertion failures across all test threads.
    inline std::mutex g_diagnostics_mutex; ///< Keeps concurrent failure diagnostics on separate lines.

    /**
     * @brief Record an expectation, including failures raised by a handler thread.
     * @param condition Whether the expectation holds.
     * @param message Diagnostic describing the expectation and relevant input.
     * @param location Caller location, supplied automatically.
     * @return Whether the expectation holds.
     */
    auto Check(
        bool condition,
        std::string_view message,
        std::source_location location = std::source_location::current()
    ) -> bool {
        if (!condition) {
            g_failures.fetch_add(1, std::memory_order_relaxed);
            std::scoped_lock lock{ g_diagnostics_mutex };
            std::cerr << location.file_name() << ':' << location.line() << ": " << message << '\n';
        }
        return condition;
    }

    /**
     * @brief Compare two values while preserving the caller's source location.
     * @tparam Actual Actual value type.
     * @tparam Expected Expected value type.
     * @param actual Observed value, borrowed for this call.
     * @param expected Expected value, borrowed for this call.
     * @param message Expectation diagnostic.
     * @param location Caller location, supplied automatically.
     * @return Whether the values compare equal.
     */
    template <typename Actual, typename Expected>
    auto CheckEq(
        Actual const& actual,
        Expected const& expected,
        std::string_view message,
        std::source_location location = std::source_location::current()
    ) -> bool {
        return Check(actual == expected, message, location);
    }

    /**
     * @brief Abort the test process if a case exceeds its deadline.
     * @note The watchdog owns its case name and joins before its synchronization state is destroyed.
     */
    class Deadline {
    private:
        std::mutex m_mutex; ///< Watchdog wait synchronization.
        std::condition_variable_any m_condition; ///< Interruptible watchdog wait.
        std::jthread m_watchdog; ///< Joined first when the deadline leaves scope.

    public:
        /**
         * @brief Start a watchdog for one test case.
         * @param name Case name, copied into the watchdog.
         * @param timeout Maximum permitted duration.
         */
        explicit Deadline(std::string_view name, std::chrono::milliseconds timeout) :
            m_watchdog{ [this, name = std::string{ name }, timeout](std::stop_token stop) {
                std::unique_lock lock{ m_mutex };
                m_condition.wait_for(lock, stop, timeout, [] { return false; });
                if (!stop.stop_requested()) {
                    std::cerr << "deadline exceeded: " << name << " (" << timeout.count()
                              << " ms)" << std::endl;
                    std::abort();
                }
            } } {}
    };

    /**
     * @brief Describe one independently runnable test case.
     */
    struct TestCase {
        std::string_view name; ///< Name, valid for the duration of RunTests.
        void (*run)(); ///< Synchronous entry point; all spawned work must join before returning.
    };

    /**
     * @brief Run matching cases, catch exceptions, and report assertion failures.
     * @param argc Argument count, including the program name.
     * @param argv Arguments; any case name containing any supplied filter is selected.
     * @param cases Borrowed test descriptors.
     * @param timeout Per-case deadline; mcpp's process timeout remains an outer bound.
     * @return Zero if at least one case ran and every expectation passed, otherwise one.
     * @note Pass filters after mcpp test's -- separator. A literal -- is ignored.
     */
    auto RunTests(
        int argc,
        char* argv[],
        std::initializer_list<TestCase> cases,
        std::chrono::milliseconds timeout = std::chrono::seconds{ 10 }
    ) -> int {
        std::size_t count{};
        for (auto const& test : cases) {
            bool filtered{};
            bool matched{};
            for (int index{ 1 }; index < argc; ++index) {
                std::string_view filter{ argv[index] };
                if (filter == "--") {
                    continue;
                }
                filtered = true;
                matched |= test.name.contains(filter);
            }
            if (filtered && !matched) {
                continue;
            }
            ++count;
            auto const before = g_failures.load(std::memory_order_relaxed);
            Deadline deadline{ test.name, timeout };
            try {
                test.run();
            } catch (std::exception const& error) {
                Check(false, std::format("{} threw: {}", test.name, error.what()));
            } catch (...) {
                Check(false, std::format("{} threw a non-standard exception", test.name));
            }
            std::println("[{}] {}", g_failures.load() == before ? "pass" : "FAIL", test.name);
        }
        if (count == 0) {
            std::cerr << "no test cases matched\n";
        }
        auto const failures = g_failures.load(std::memory_order_relaxed);
        std::println("{} case(s), {} failure(s)", count, failures);
        return count != 0 && failures == 0 ? 0 : 1;
    }

} // namespace mhttp::test
