/**
 * @file thread_task.hpp
 * @brief Run test-only blocking callers on owned threads with exception-aware results.
 */
#pragma once

namespace mhttp::test {

    /**
     * @brief Own a dedicated caller thread independently of the platform's std::async scheduler.
     * @tparam T Owned result type, including void.
     */
    template <typename T>
    class ThreadTask {
    private:
        std::future<T> m_future; ///< Result or exception published by packaged_task.
        std::jthread m_thread; ///< Joined before the result state is destroyed.

    public:
        /**
         * @brief Start an owned non-coroutine entry point on a test-only blocking caller thread.
         * @tparam F Callable type.
         * @param function Owned callable; referenced fixtures must outlive this ThreadTask.
         */
        template <typename F>
        explicit ThreadTask(F function) {
            std::packaged_task<T()> operation{ std::move(function) };
            m_future = operation.get_future();
            m_thread = std::jthread{ [operation = std::move(operation)]() mutable { operation(); } };
        }
        /**
         * @brief Collect the result or rethrow its original exception.
         * @return Owned value, or void.
         */
        auto Get() -> T { return m_future.get(); }
        /**
         * @brief Wait from the test caller for bounded result readiness.
         * @param duration Maximum blocking interval.
         * @return Future readiness state.
         */
        auto WaitFor(std::chrono::milliseconds duration) -> std::future_status { return m_future.wait_for(duration); }
    };

    /**
     * @brief Deduce the result of a test-only caller function.
     * @tparam F Callable type.
     * @param function Callable used for type deduction.
     */
    template <typename F>
    ThreadTask(F function) -> ThreadTask<std::invoke_result_t<F>>;

} // namespace mhttp::test
