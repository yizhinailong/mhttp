/**
 * @file task.cppm
 * @brief Own lazy coroutine frames and transfer execution symmetrically without exposing Asio.
 */
export module mhttp:core.task;

import std;

export namespace mhttp {

    /**
     * @brief Own a lazy, single-consumer coroutine returning a value or void.
     * @tparam T Owned result type; references are not supported.
     * @note Move a Task into co_await. Its frame and any captured parameters survive until the
     *       await expression ends. Before awaiting, destruction releases the unstarted frame.
     *       Keep objects borrowed by coroutine parameters alive until the operation completes.
     */
    template <typename T = void>
    class [[nodiscard]] Task;

} // namespace mhttp

export namespace mhttp::detail {

    /**
     * @brief Erase the executor used to resume a suspended Task after external completion.
     * @note Schedule must enqueue the handle, never resume it inline. The owning runtime must
     *       keep scheduled frames alive until queued execution and outstanding operations drain.
     * @internal
     */
    class Scheduler {
    public:
        /**
         * @brief Destroy the scheduler after its tasks release shared ownership.
         */
        virtual ~Scheduler() = default;

        /**
         * @brief Enqueue exactly one resumption on this scheduler's execution context.
         * @param continuation Borrowed suspended frame, owned until the queued resumption completes.
         */
        virtual auto Schedule(std::coroutine_handle<> continuation) -> void = 0;
    };

    /**
     * @brief Transfer from a finished child directly to its suspended parent.
     * @internal
     */
    struct TaskFinalAwaiter {
        /**
         * @brief Always suspend so the owner can destroy the finished frame.
         * @return False.
         */
        [[nodiscard]] auto await_ready() const noexcept -> bool { return false; }

        /**
         * @brief Return the continuation handle for symmetric transfer.
         * @tparam Promise Task promise type.
         * @param frame Borrowed finishing frame, still owned by its awaiter or root operation.
         * @return Parent handle, or a no-op handle for a root.
         */
        template <typename Promise>
        auto await_suspend(
            std::coroutine_handle<Promise> frame
        ) const noexcept -> std::coroutine_handle<> {
            return frame.promise().continuation;
        }

        /**
         * @brief Satisfy the final awaiter protocol; a finished frame is never resumed.
         */
        auto await_resume() const noexcept -> void {}
    };

    /**
     * @brief Share continuation, exception storage, and scheduler ownership across Task promises.
     * @internal
     */
    struct TaskPromiseBase {
        std::shared_ptr<Scheduler> scheduler; ///< Executor inherited by child tasks.
        std::coroutine_handle<> continuation{ std::noop_coroutine() }; ///< Suspended parent or no-op.
        std::exception_ptr exception; ///< Exception rethrown in the awaiting coroutine.

        /**
         * @brief Suspend before entering the coroutine body.
         * @return Lazy initial suspension.
         */
        [[nodiscard]] auto initial_suspend() const noexcept -> std::suspend_always { return {}; }

        /**
         * @brief Preserve frame ownership and transfer directly to the parent at completion.
         * @return Symmetric final suspension.
         */
        [[nodiscard]] auto final_suspend() const noexcept -> TaskFinalAwaiter { return {}; }

        /**
         * @brief Capture a body or return-value exception for the awaiting coroutine.
         */
        auto unhandled_exception() noexcept -> void { exception = std::current_exception(); }
    };

    /**
     * @brief Store an owned result without requiring default construction.
     * @tparam T Owned value type.
     * @internal
     */
    template <typename T>
    struct TaskPromise : TaskPromiseBase {
        std::optional<T> value; ///< Result materialized by co_return.

        /**
         * @brief Create the sole owner of this coroutine frame.
         * @return Lazy Task.
         */
        auto get_return_object() noexcept -> Task<T>;

        /**
         * @brief Construct the result in its promise storage.
         * @tparam U Convertible result expression type.
         * @param result Value forwarded into owned storage.
         */
        template <typename U>
            requires std::constructible_from<T, U&&>
        auto return_value(U&& result) -> void { value.emplace(std::forward<U>(result)); }
    };

    /**
     * @brief Complete a Task without a value while retaining exception propagation.
     * @internal
     */
    template <>
    struct TaskPromise<void> : TaskPromiseBase {
        /**
         * @brief Create the sole owner of this coroutine frame.
         * @return Lazy void Task.
         */
        auto get_return_object() noexcept -> Task<void>;

        /**
         * @brief Record successful void completion.
         */
        auto return_void() const noexcept -> void {}
    };

    struct TaskAccess;

} // namespace mhttp::detail

export namespace mhttp {

    template <typename T>
    class [[nodiscard]] Task {
        static_assert(std::is_void_v<T> || (std::is_object_v<T> && std::move_constructible<T>),
            "Task results must be void or movable owned objects");

    public:
        using promise_type = detail::TaskPromise<T>; ///< Standard coroutine promise customization.

    private:
        using Handle = std::coroutine_handle<promise_type>; ///< Typed owned frame handle.
        Handle m_frame; ///< Lazy frame, or empty after move/co_await.

        friend promise_type;
        friend struct detail::TaskAccess;

        /**
         * @brief Adopt the coroutine frame created by its promise.
         * @param frame Owned suspended frame.
         */
        explicit Task(Handle frame) noexcept : m_frame{ frame } {}

    public:
        /**
         * @brief Construct an empty Task, which throws if awaited.
         */
        Task() noexcept = default;

        /**
         * @brief Transfer sole frame ownership without starting execution.
         * @param other Task left empty.
         */
        Task(Task&& other) noexcept : m_frame{ std::exchange(other.m_frame, {}) } {}

        /**
         * @brief Destroy any unstarted frame and transfer ownership from another Task.
         * @param other Task left empty, unless self-assigned.
         * @return This owner.
         */
        auto operator=(Task&& other) noexcept -> Task& {
            if (this != &other) {
                if (m_frame) { m_frame.destroy(); }
                m_frame = std::exchange(other.m_frame, {});
            }
            return *this;
        }

        /**
         * @brief Destroy an unstarted frame or the completed root owned by the runtime.
         * @note Internal root owners must drain asynchronous resumptions before destruction.
         */
        ~Task() { if (m_frame) { m_frame.destroy(); } }

        /**
         * @brief Observe whether this object owns an unconsumed frame.
         * @return False for empty, moved, or awaited objects.
         */
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_frame); }

        /**
         * @brief Own a child frame for exactly one await expression.
         * @note Destroy only after completion, or when no external operation can resume the frame.
         * @internal
         */
        class Awaiter {
        private:
            Handle m_frame; ///< Sole owner transferred from the Task expression.

        public:
            /**
             * @brief Adopt the consumed Task's frame.
             * @param frame Owned suspended frame, possibly empty.
             */
            explicit Awaiter(Handle frame) noexcept : m_frame{ frame } {}

            /**
             * @brief Prevent duplicate ownership of a consumed coroutine frame.
             * @param other Unused source.
             */
            Awaiter(Awaiter const& other) = delete;

            /**
             * @brief Prevent replacement of a frame after publishing its continuation.
             * @param other Unused source.
             * @return Not callable.
             */
            auto operator=(Awaiter const& other) -> Awaiter& = delete;

            /**
             * @brief Destroy the child frame after consuming its value or exception.
             */
            ~Awaiter() { if (m_frame) { m_frame.destroy(); } }

            /**
             * @brief Skip suspension only for an empty Task, so await_resume can report misuse.
             * @return Whether there is no child to run.
             */
            [[nodiscard]] auto await_ready() const noexcept -> bool { return !m_frame; }

            /**
             * @brief Inherit the parent scheduler and transfer execution to the child.
             * @tparam ParentPromise Promise of the awaiting coroutine.
             * @param parent Borrowed suspended parent, alive until child completion.
             * @return Child handle for symmetric transfer without recursive resume calls.
             */
            template <typename ParentPromise>
            auto await_suspend(
                std::coroutine_handle<ParentPromise> parent
            ) const noexcept -> std::coroutine_handle<> {
                auto& promise = m_frame.promise();
                promise.continuation = parent;
                if constexpr (std::derived_from<ParentPromise, detail::TaskPromiseBase>) {
                    promise.scheduler = parent.promise().scheduler;
                }
                return m_frame;
            }

            /**
             * @brief Consume the child result or rethrow its original exception.
             * @return Owned result for non-void Tasks.
             * @throws std::logic_error If the Task was empty or already consumed.
             */
            auto await_resume() const -> T {
                if (!m_frame) { throw std::logic_error{ "cannot await an empty Task" }; }
                auto& promise = m_frame.promise();
                if (promise.exception) { std::rethrow_exception(promise.exception); }
                if constexpr (!std::is_void_v<T>) { return std::move(*promise.value); }
            }
        };

        /**
         * @brief Consume this Task into an awaiter; lvalues must be explicitly moved.
         * @return Awaiter owning the coroutine frame.
         */
        [[nodiscard]] auto operator co_await() && noexcept -> Awaiter {
            return Awaiter{ std::exchange(m_frame, {}) };
        }
    };

} // namespace mhttp

export namespace mhttp::detail {

    /**
     * @brief Adopt a value promise's newly allocated coroutine frame.
     * @tparam T Owned result type.
     * @return Sole lazy Task owner.
     * @internal
     */
    template <typename T>
    auto TaskPromise<T>::get_return_object() noexcept -> Task<T> {
        return Task<T>{ std::coroutine_handle<TaskPromise>::from_promise(*this) };
    }

    /**
     * @brief Adopt a void promise's newly allocated coroutine frame.
     * @return Sole lazy Task owner.
     * @internal
     */
    inline auto TaskPromise<void>::get_return_object() noexcept -> Task<void> {
        return Task<void>{ std::coroutine_handle<TaskPromise>::from_promise(*this) };
    }

    /**
     * @brief Let runtime root owners bind a scheduler and resume their still-owned Task frame.
     * @note These operations require an unstarted root, or a root whose lifetime is controlled by
     *       the caller until final suspension. They do not make arbitrary frame destruction safe.
     * @internal
     */
    struct TaskAccess {
        /**
         * @brief Borrow a root handle without transferring ownership.
         * @tparam T Root result type.
         * @param task Root owned until completion and destruction.
         * @return Borrowed typed handle, possibly empty.
         */
        template <typename T>
        [[nodiscard]] static auto Handle(Task<T> const& task) noexcept {
            return task.m_frame;
        }

        /**
         * @brief Bind an unstarted nonempty root to its executor.
         * @tparam T Root result type.
         * @param task Owned unstarted root.
         * @param scheduler Shared scheduler propagated to awaited child tasks.
         */
        template <typename T>
        static auto Bind(Task<T>& task, std::shared_ptr<Scheduler> scheduler) noexcept -> void {
            task.m_frame.promise().scheduler = std::move(scheduler);
        }
    };

} // namespace mhttp::detail
