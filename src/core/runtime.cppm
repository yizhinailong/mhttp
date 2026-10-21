/**
 * @file runtime.cppm
 * @brief Expose an Asio-free owner of I/O execution threads and orderly stopping.
 */
export module mhttp:core.runtime;

import std;
import :core.impl.runtime;
import :core.impl.bridge;
export import :core.error;
export import :core.task;

export namespace mhttp::detail {

    struct RuntimeAccess;

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Own the execution context shared by HTTP servers, clients, and asynchronous Tasks.
     * @note Runtime must outlive all servers, clients, and operations created from it. Stop/Join
     *       and destruction must complete before their borrowed Runtime reference expires.
     *       Destroy Runtime on a non-I/O thread. Copying and moving are disabled to keep identity stable.
     */
    class Runtime {
    private:
        std::shared_ptr<impl::RuntimeImpl> m_impl; ///< Owned context and thread lifecycle.
        friend struct detail::RuntimeAccess;

    public:
        /**
         * @brief Create an idle runtime without starting any execution threads.
         */
        Runtime() : m_impl{ std::make_shared<impl::RuntimeImpl>() } {}

        /**
         * @brief Keep runtime identity and ownership unique.
         * @param other Unused source.
         */
        Runtime(Runtime const& other) = delete;

        /**
         * @brief Prevent replacement of a context borrowed by active servers or clients.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(Runtime const& other) -> Runtime& = delete;

        /**
         * @brief Request cancellation and join all execution threads before releasing the context.
         * @note Destruction on an I/O thread violates ownership and terminates instead of detaching
         *       a thread that could access the destroyed context.
         */
        ~Runtime() {
            Stop();
            if (impl::RuntimeImpl::IsIoThread()) { std::terminate(); }
            static_cast<void>(Join());
        }

        /**
         * @brief Start a fresh generation on owned I/O threads without blocking.
         * @param thread_count Positive thread count, defaulting to one.
         * @return Success, InvalidRequest for zero, or AlreadyRunning until the old generation joins.
         * @throws std::system_error If the operating system cannot create a thread.
         */
        [[nodiscard]] auto Start(std::size_t thread_count = 1) -> Result<void> {
            return m_impl->Start(thread_count);
        }

        /**
         * @brief Run a fresh generation on this thread until stopped and drained.
         * @return Success, AlreadyRunning, Busy on an I/O thread, or Unknown after a callback failure.
         * @note Run and Start select separate modes. Call Join after Run returns before restarting.
         */
        [[nodiscard]] auto Run() -> Result<void> {
            auto implementation = m_impl;
            return implementation->Run();
        }

        /**
         * @brief Request cancellation from any thread, leaving completion handlers able to run.
         * @note Nonblocking and idempotent. Follow with Join on a non-I/O thread to wait for drain.
         */
        auto Stop() -> void { m_impl->Stop(); }

        /**
         * @brief Wait for all runners and permit another Start or Run.
         * @return Success, Busy on an I/O thread, or Unknown after a callback failure.
         * @note Call Stop first. Repeated calls are safe; arbitrary blocking work must finish itself.
         */
        [[nodiscard]] auto Join() -> Result<void> { return m_impl->Join(); }

        /**
         * @brief Observe whether this runtime currently accepts new operations.
         * @return Whether started and not yet stopping.
         */
        [[nodiscard]] auto IsRunning() const -> bool { return m_impl->IsRunning(); }

        /**
         * @brief Detect I/O execution on any mhttp runtime.
         * @return Whether this thread must avoid blocking waits and blocking application work.
         */
        [[nodiscard]] static auto IsIoThread() noexcept -> bool { return impl::RuntimeImpl::IsIoThread(); }
    };

} // namespace mhttp

export namespace mhttp::detail {

    /**
     * @brief Submit runtime roots through standard types without exposing an executor.
     * @internal
     */
    struct RuntimeAccess {
        /**
         * @brief Start a root only if the runtime still accepts operations.
         * @param runtime Borrowed runtime, alive through completion and drain.
         * @param task Consumed root Task.
         * @param complete Owned exception-aware completion, called after Task frame destruction.
         * @return Success or NotRunning before execution.
         */
        static auto Submit(
            Runtime& runtime,
            Task<void> task,
            std::function<void(std::exception_ptr)> complete
        ) -> Result<void> {
            return impl::submit_task(runtime.m_impl, std::move(task), std::move(complete));
        }
    };

    /**
     * @brief Own a blocking caller's completion state and result awaiting root cleanup.
     * @tparam T Owned result or void.
     * @internal
     */
    template <typename T>
    struct SyncWaitState {
        std::mutex mutex; ///< Completion publication, paired with the caller's blocking wait.
        std::condition_variable ready; ///< Wakes only after all user Task frames are destroyed.
        bool completed{}; ///< Predicate published under mutex.
        std::exception_ptr failure; ///< Original user exception, if any.
        std::optional<std::conditional_t<std::is_void_v<T>, std::monostate, T>> value; ///< Owned pending value.
    };

    /**
     * @brief Store a value without waking the blocking caller before frame cleanup.
     * @tparam T Owned result or void.
     * @param task Consumed user Task.
     * @param state Shared storage retained by the completion callback.
     * @internal
     */
    template <typename T>
    auto collect_wait_result(Task<T> task, std::shared_ptr<SyncWaitState<T>> state) -> Task<void> {
        if constexpr (std::is_void_v<T>) { co_await std::move(task); }
        else { state->value.emplace(co_await std::move(task)); }
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Run a Task on a started Runtime and block this non-I/O caller for its result.
     * @tparam T Owned result or void.
     * @param runtime Started runtime, alive through the wait and completion cleanup.
     * @param task Consumed lazy user Task.
     * @return Owned result, after all user coroutine frames have been destroyed.
     * @throws std::system_error With Busy on any I/O thread, or NotRunning for an idle/stopping runtime.
     * @note User exceptions propagate unchanged. Never use this for a wait on an I/O execution thread.
     */
    template <typename T>
    auto SyncWait(Runtime& runtime, Task<T> task) -> T {
        if (Runtime::IsIoThread()) { impl::throw_error(Error::Busy); }
        auto state = std::make_shared<detail::SyncWaitState<T>>();
        auto submitted = detail::RuntimeAccess::Submit(runtime,
            detail::collect_wait_result(std::move(task), state), [state](std::exception_ptr failure) {
                {
                    std::lock_guard lock{ state->mutex };
                    state->failure = failure;
                    state->completed = true;
                }
                state->ready.notify_one();
            });
        if (!submitted) { impl::throw_error(submitted.error()); }
        std::unique_lock lock{ state->mutex };
        state->ready.wait(lock, [&state] { return state->completed; });
        if (state->failure) { std::rethrow_exception(state->failure); }
        if constexpr (!std::is_void_v<T>) { return std::move(*state->value); }
    }

    /**
     * @brief Suspend without blocking an I/O thread, resuming on the awaiting Task's strand.
     * @param duration Relative delay; zero/negative durations complete asynchronously.
     * @return Lazy timer Task.
     * @throws std::system_error With Canceled when the owning runtime/connection stops.
     */
    [[nodiscard]] auto Sleep(std::chrono::steady_clock::duration duration) -> Task<void> {
        return impl::sleep_for(duration);
    }

    /**
     * @brief Execute blocking work on a separate pool, returning to this Task's original strand.
     * @tparam Function Owned invocable; move-only callables and results are supported.
     * @param function Function moved into the lazy Task frame, invoked once on a worker thread.
     * @return Owned function result, or void. References are not supported.
     * @throws std::system_error With Canceled when cancellation precedes invocation or follows it.
     * @note A running function cannot be interrupted. Stop returns promptly; Join waits for it.
     *       Keep anything borrowed by function alive until the Task completes. Exceptions propagate
     *       unchanged, and execution resumes on the original strand for success and failure.
     */
    template <typename Function> requires std::invocable<Function&>
    [[nodiscard]] auto Offload(Function function) -> Task<std::invoke_result_t<Function&>> {
        using Value = std::invoke_result_t<Function&>;
        static_assert(!std::is_reference_v<Value>, "Offload must return an owned value or void");
        std::optional<std::conditional_t<std::is_void_v<Value>, std::monostate, Value>> result;
        co_await impl::offload([&function, &result] {
            if constexpr (std::is_void_v<Value>) { std::invoke(function); }
            else { result.emplace(std::invoke(function)); }
        });
        if constexpr (!std::is_void_v<Value>) { co_return std::move(*result); }
    }

} // namespace mhttp
