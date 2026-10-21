/**
 * @file asio_bridge.cppm
 * @brief Bridge owned Tasks and Asio coroutines with serialized resumption and cancellation.
 */
module mhttp:core.impl.bridge;

import std;
import asio;
import :core.task;
import :core.error;
import :core.impl.runtime;

namespace mhttp::impl {

    /**
     * @brief Own a root Task until its last resumption has returned to the executor.
     */
    struct TaskRoot {
        Task<void> task; ///< Root whose children may resume on this root's scheduler.
        std::function<void(std::exception_ptr)> complete; ///< Called once after destroying all Task frames.

        /**
         * @brief Resume a root or child and settle only after the root reaches final suspension.
         * @param continuation Borrowed handle owned by this root's coroutine tree.
         */
        auto Resume(std::coroutine_handle<> continuation) -> void {
            continuation.resume();
            auto const frame = detail::TaskAccess::Handle(task);
            if (!frame.done()) { return; }
            auto failure = frame.promise().exception;
            task = {};
            auto completion = std::move(complete);
            completion(failure);
        }
    };

    /**
     * @brief Keep one root's external completions on its original strand.
     */
    class AsioScheduler : public detail::Scheduler {
    private:
        asio::any_io_executor m_executor; ///< Root strand, also used for completion observation.
        std::stop_token m_stop; ///< Connection or runtime generation cancellation.
        std::weak_ptr<TaskRoot> m_root; ///< Root held by its awaiting Asio coroutine, avoiding a cycle.

    public:
        /**
         * @brief Bind to an existing serialized executor and an owned root's lifetime.
         * @param executor Strand on which the root starts and resumes.
         * @param stop Cancellation token for all nested operations.
         * @param root Root retained by AwaitTask until completion.
         */
        AsioScheduler(
            asio::any_io_executor executor,
            std::stop_token stop,
            std::weak_ptr<TaskRoot> root
        ) :
            m_executor{ std::move(executor) }, m_stop{ stop }, m_root{ std::move(root) } {}

        /**
         * @brief Queue a root or child resumption, retaining the root through the callback.
         * @param continuation Borrowed suspended handle; AwaitTask retains its owning root.
         */
        auto Schedule(std::coroutine_handle<> continuation) -> void override {
            asio::post(m_executor, [root = m_root.lock(), continuation] { root->Resume(continuation); });
        }

        /**
         * @brief Borrow the root executor for timers and resumed continuations.
         * @return Owned copy of the root strand executor.
         */
        [[nodiscard]] auto Executor() const -> asio::any_io_executor { return m_executor; }

        /**
         * @brief Observe connection/runtime cancellation for a nested I/O operation.
         * @return Stop token valid across the root's lifetime.
         */
        [[nodiscard]] auto StopToken() const noexcept -> std::stop_token { return m_stop; }
    };

    /**
     * @brief Carry an Asio-free result between the Task tree and its awaiting Asio coroutine.
     * @tparam T Owned result or void.
     */
    template <typename T>
    struct AwaitTaskState {
        using Stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>; ///< Void storage adapter.
        std::optional<Stored> value; ///< Result stored before root final suspension.
        std::exception_ptr failure; ///< Original Task exception.
        std::shared_ptr<TaskRoot> root; ///< Root owner until its completion notification is received.
        asio::experimental::channel<void(asio::error_code)> done; ///< Buffered, single completion.
        asio::cancellation_signal neutral; ///< Never emitted; cancellation must drain the Task first.
        decltype(asio::make_work_guard(std::declval<asio::any_io_executor>())) work; ///< Keep root callbacks runnable.

        /**
         * @brief Keep the root executor alive and create its one-element completion channel.
         * @param executor Serialized root executor.
         */
        explicit AwaitTaskState(asio::any_io_executor executor) :
            done{ executor, 1 }, work{ asio::make_work_guard(executor) } {}
    };

    /**
     * @brief Collect a user's Task result before root completion destroys its last frame.
     * @tparam T Owned result or void.
     * @param task Consumed user Task.
     * @param state Borrowed result storage retained by AwaitTask.
     */
    template <typename T>
    auto collect_task(Task<T> task, AwaitTaskState<T>& state) -> Task<void> {
        if constexpr (std::is_void_v<T>) { co_await std::move(task); }
        else { state.value.emplace(co_await std::move(task)); }
    }

    /**
     * @brief Await a complete Task tree without exposing Asio to the Task's public interface.
     * @tparam T Owned result or void.
     * @param task Consumed user coroutine.
     * @param executor Serialized executor for all user code in this root.
     * @param stop Connection/runtime stop token; cancellation completes nested operations before drain.
     * @return User result, rethrowing its exception after frame cleanup.
     */
    template <typename T>
    auto AwaitTask(
        Task<T> task,
        asio::any_io_executor executor,
        std::stop_token stop = {}
    ) -> asio::awaitable<T> {
        auto state = std::make_shared<AwaitTaskState<T>>(executor);
        state->root = std::make_shared<TaskRoot>();
        state->root->task = collect_task(std::move(task), *state);
        state->root->complete = [weak = std::weak_ptr{ state }](std::exception_ptr failure) {
            auto owner = weak.lock();
            owner->failure = failure;
            static_cast<void>(owner->done.try_send(asio::error_code{}));
        };
        auto scheduler = std::make_shared<AsioScheduler>(executor, stop, state->root);
        detail::TaskAccess::Bind(state->root->task, scheduler);
        scheduler->Schedule(detail::TaskAccess::Handle(state->root->task));
        bool const previous = co_await asio::this_coro::throw_if_cancelled();
        co_await asio::this_coro::throw_if_cancelled(false);
        co_await state->done.async_receive(asio::bind_cancellation_slot(state->neutral.slot(), asio::use_awaitable));
        co_await asio::this_coro::throw_if_cancelled(previous);
        if (state->failure) { std::rethrow_exception(state->failure); }
        if constexpr (!std::is_void_v<T>) { co_return std::move(*state->value); }
    }

    /**
     * @brief Box an Asio result so co_spawn does not require default construction of user values.
     * @tparam T Owned operation result or void.
     * @param operation Consumed Asio coroutine.
     * @return Engaged optional, using monostate for void.
     */
    template <typename T>
    auto box_operation(
        asio::awaitable<T> operation
    ) -> asio::awaitable<std::optional<std::conditional_t<std::is_void_v<T>, std::monostate, T>>> {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(operation);
            co_return std::optional{ std::monostate{} };
        } else { co_return std::optional<T>{ co_await std::move(operation) }; }
    }

    /**
     * @brief Retain an operation's cancellation signal independently of its awaiting Task frame.
     * @tparam T Owned result or void.
     * @note Only the operation strand accesses completed, signal, stop_callback, and results.
     *       Queued cancellation retains this state and becomes harmless after completion.
     */
    template <typename T>
    struct AsioOperationState {
        using Stored = std::conditional_t<std::is_void_v<T>, std::monostate, T>; ///< Void storage adapter.
        asio::any_io_executor executor; ///< Separate strand serializing initiation, completion, and cancel.
        std::shared_ptr<AsioScheduler> scheduler; ///< Original Task strand and stop generation.
        asio::cancellation_signal signal; ///< Per-operation co_spawn cancellation.
        std::optional<std::stop_callback<std::function<void()>>> stop_callback; ///< Posts cancellation to the operation strand.
        std::optional<Stored> value; ///< Successful result, published through Scheduler::Schedule.
        std::exception_ptr failure; ///< Operation or result-construction failure.
        bool completed{}; ///< Suppress cancellation callbacks queued after completion.

        /**
         * @brief Serialize operation internals even when supplied a plain multithreaded executor.
         * @param target Executor used to launch the Asio operation.
         */
        explicit AsioOperationState(asio::any_io_executor target) :
            executor{ asio::make_strand(target) } {}
    };

    /**
     * @brief Suspend a Task while an independently owned Asio operation runs and cancels safely.
     * @tparam T Owned result or void.
     */
    template <typename T>
    class AsioOperationAwaiter {
    private:
        asio::awaitable<T> m_operation; ///< Owned unstarted operation.
        std::shared_ptr<AsioOperationState<T>> m_state; ///< Result and cancellation lifetime.

    public:
        /**
         * @brief Adopt an operation without starting it.
         * @param executor Target executor, wrapped in an operation strand.
         * @param operation Consumed Asio coroutine.
         */
        AsioOperationAwaiter(
            asio::any_io_executor executor,
            asio::awaitable<T> operation
        ) :
            m_operation{ std::move(operation) }, m_state{ std::make_shared<AsioOperationState<T>>(executor) } {}

        /**
         * @brief Always suspend for asynchronous initiation.
         * @return False.
         */
        auto await_ready() const noexcept -> bool { return false; }

        /**
         * @brief Initiate on the operation strand, then resume the Task on its inherited scheduler.
         * @tparam Promise Awaiting Task promise.
         * @param parent Borrowed Task frame owned until completion by AwaitTask's root.
         */
        template <typename Promise>
        auto await_suspend(std::coroutine_handle<Promise> parent) -> void {
            m_state->scheduler = std::dynamic_pointer_cast<AsioScheduler>(parent.promise().scheduler);
            if (!m_state->scheduler) { throw std::logic_error{ "Task requires an mhttp runtime scheduler" }; }
            asio::post(m_state->executor, [state = m_state, operation = std::move(m_operation), parent]() mutable {
                try {
                    state->stop_callback.emplace(state->scheduler->StopToken(), [weak = std::weak_ptr{ state }] {
                        if (auto owner = weak.lock()) {
                            asio::post(owner->executor, [owner] {
                                if (!owner->completed) { owner->signal.emit(asio::cancellation_type::all); }
                            });
                        }
                    });
                    asio::co_spawn(state->executor, box_operation(std::move(operation)),
                        asio::bind_cancellation_slot(state->signal.slot(),
                            [state, parent](std::exception_ptr failure, std::optional<typename AsioOperationState<T>::Stored> value) {
                                state->completed = true;
                                state->stop_callback.reset();
                                state->failure = failure;
                                if (!failure) {
                                    try { state->value.emplace(std::move(*value)); }
                                    catch (...) { state->failure = std::current_exception(); }
                                }
                                state->scheduler->Schedule(parent);
                            }));
                } catch (...) {
                    state->completed = true;
                    state->stop_callback.reset();
                    state->failure = std::current_exception();
                    state->scheduler->Schedule(parent);
                }
            });
        }

        /**
         * @brief Consume the operation result on the original Task strand.
         * @return Owned result, if non-void.
         * @throws std::system_error With mhttp Canceled for operation_aborted.
         */
        auto await_resume() -> T {
            if (m_state->failure) {
                try { std::rethrow_exception(m_state->failure); }
                catch (std::system_error const& error) {
                    if (error.code() == asio::error::operation_aborted) {
                        throw_error(Error::Canceled);
                    }
                    throw;
                }
            }
            if constexpr (!std::is_void_v<T>) { return std::move(*m_state->value); }
        }
    };

    /**
     * @brief Present an Asio operation as a lazy Task with strand-preserving completion.
     * @tparam T Owned result or void.
     * @param executor Executor on which the Asio operation starts.
     * @param operation Consumed Asio coroutine.
     * @return User result on the awaiting Task's inherited scheduler.
     */
    template <typename T>
    auto ToTask(asio::any_io_executor executor, asio::awaitable<T> operation) -> Task<T> {
        co_return co_await AsioOperationAwaiter<T>{ std::move(executor), std::move(operation) };
    }

    /**
     * @brief Read the scheduler of the currently executing Task without suspending it.
     */
    struct CurrentAsioScheduler {
        std::shared_ptr<AsioScheduler> scheduler; ///< Original strand inherited by this Task.

        /**
         * @brief Inspect the promise through await_suspend.
         * @return False.
         */
        auto await_ready() const noexcept -> bool { return false; }

        /**
         * @brief Copy the inherited scheduler and continue inline.
         * @tparam Promise Awaiting Task promise.
         * @param frame Borrowed currently executing Task.
         * @return False, leaving the Task running.
         */
        template <typename Promise>
        auto await_suspend(std::coroutine_handle<Promise> frame) noexcept -> bool {
            scheduler = std::dynamic_pointer_cast<AsioScheduler>(frame.promise().scheduler);
            return false;
        }

        /**
         * @brief Require a runtime-backed scheduler for I/O helpers.
         * @return Shared original strand scheduler.
         * @throws std::logic_error If executed outside an mhttp asynchronous root.
         */
        auto await_resume() -> std::shared_ptr<AsioScheduler> {
            if (!scheduler) { throw std::logic_error{ "Task requires an mhttp runtime scheduler" }; }
            return std::move(scheduler);
        }
    };

    /**
     * @brief Wait on an owned timer, letting co_spawn cancellation cancel its pending wait.
     * @param duration Delay relative to operation initiation.
     */
    auto wait_timer(std::chrono::steady_clock::duration duration) -> asio::awaitable<void> {
        asio::steady_timer timer{ co_await asio::this_coro::executor };
        timer.expires_after(duration);
        co_await timer.async_wait(asio::use_awaitable);
    }

    /**
     * @brief Adapt a cancellable timer to the current Task scheduler.
     * @param duration Delay; zero or negative durations still complete asynchronously.
     */
    auto sleep_for(std::chrono::steady_clock::duration duration) -> Task<void> {
        auto scheduler = co_await CurrentAsioScheduler{};
        co_await ToTask(scheduler->Executor(), wait_timer(duration));
    }

    /**
     * @brief Submit a root to a running generation and report all exceptions to its completion.
     * @param runtime Runtime retained through initiation; its public owner must drain before destruction.
     * @param task Consumed lazy root.
     * @param complete Owned callback invoked after all Task frames have been destroyed.
     * @return Success or NotRunning before any coroutine starts.
     */
    auto submit_task(
        std::shared_ptr<RuntimeImpl> runtime,
        Task<void> task,
        std::function<void(std::exception_ptr)> complete
    ) -> Result<void> {
        auto root = std::make_shared<TaskRoot>();
        root->task = std::move(task);
        root->complete = std::move(complete);
        return runtime->Submit([runtime, root](std::stop_token stop) mutable {
            try {
                auto executor = asio::make_strand(runtime->Context());
                asio::co_spawn(executor, AwaitTask(std::move(root->task), executor, stop),
                    [root](std::exception_ptr failure) { root->complete(failure); });
            } catch (...) {
                root->task = {};
                root->complete(std::current_exception());
            }
        });
    }

} // namespace mhttp::impl
