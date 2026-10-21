/**
 * @file runtime.cppm
 * @brief Expose an Asio-free owner of I/O execution threads and orderly stopping.
 */
export module mhttp:core.runtime;

import std;
import :core.impl.runtime;
export import :core.error;

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
