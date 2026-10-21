/**
 * @file runtime_impl.cppm
 * @brief Manage Asio execution threads and drain cancellation completions before restart.
 */
module mhttp:core.impl.runtime;

import std;
import asio;
import :core.error;
import :core.impl.io_error;

namespace mhttp::impl {

    class RuntimeImpl;
    inline thread_local RuntimeImpl* g_current_runtime{}; ///< Runtime whose I/O callback is executing.

    /**
     * @brief Own the I/O context, work guard, stop generation, and joined execution threads.
     * @note Lifecycle fields use m_mutex. Joiners serialize with m_join_mutex and leave the state
     *       non-idle until every runner exits, so restart never races io_context::run or Join.
     */
    class RuntimeImpl {
    private:
        /**
         * @brief Prevent restart until a stopped generation has been joined.
         */
        enum class State { Idle, Running, Stopping, Drained };

        asio::io_context m_context; ///< Context reused only after all runners have drained.
        using WorkGuard = decltype(asio::make_work_guard(std::declval<asio::io_context&>()));
        std::optional<WorkGuard> m_work; ///< Keep the context alive until cancellation is posted.
        std::stop_source m_stop_source; ///< Cancellation source for this generation.
        mutable std::mutex m_mutex; ///< State, runner count, stop source, and thread vector.
        std::mutex m_join_mutex; ///< One joiner owns the extracted thread vector at a time.
        std::condition_variable m_drained; ///< Notify inline runners and concurrent joiners.
        State m_state{ State::Idle }; ///< Current generation lifecycle.
        std::vector<std::thread> m_threads; ///< Threads owned until extracted by Join.
        std::shared_ptr<asio::thread_pool> m_workers; ///< Lazily created pool, reused across drained generations.
        std::size_t m_runners{}; ///< All run loops, including an inline Run.
        bool m_failed{}; ///< An unexpected callback exception stopped this generation.

    public:
        /**
         * @brief Start an idle context on owned execution threads.
         * @param thread_count Positive I/O thread count.
         * @return Success, InvalidRequest for zero, or AlreadyRunning until the old generation joins.
         */
        auto Start(std::size_t thread_count) -> Result<void> {
            std::unique_lock lock{ m_mutex };
            if (thread_count == 0) { return std::unexpected{ Error::InvalidRequest }; }
            if (m_state != State::Idle) { return std::unexpected{ Error::AlreadyRunning }; }
            prepare();
            try {
                m_threads.reserve(thread_count);
                for (std::size_t i{}; i < thread_count; ++i) {
                    m_threads.emplace_back([this] { static_cast<void>(runLoop()); });
                    ++m_runners;
                }
            } catch (...) {
                auto const failure = std::current_exception();
                lock.unlock();
                Stop();
                static_cast<void>(Join());
                std::rethrow_exception(failure);
            }
            return {};
        }

        /**
         * @brief Execute a fresh generation on the calling non-I/O thread until Stop and drain.
         * @param initial Optional owned initialization posted after publishing this fresh generation.
         * @return Success, AlreadyRunning, Busy on an I/O thread, or Unknown for a callback failure.
         */
        auto Run(std::function<void(std::stop_token)> initial = {}) -> Result<void> {
            if (IsIoThread()) { return std::unexpected{ Error::Busy }; }
            {
                std::lock_guard lock{ m_mutex };
                if (m_state != State::Idle) { return std::unexpected{ Error::AlreadyRunning }; }
                prepare();
                try {
                    if (initial) {
                        asio::post(m_context, [initial = std::move(initial), token = m_stop_source.get_token()] {
                            initial(token);
                        });
                    }
                } catch (...) {
                    m_work.reset();
                    m_state = State::Idle;
                    throw;
                }
                ++m_runners;
            }
            return runLoop();
        }

        /**
         * @brief Request cancellation without blocking or discarding queued completion handlers.
         * @note Callbacks registered on StopToken must only post cancellation to their owner executor.
         *       Join waits for those completions and any already executing blocking work to drain.
         */
        auto Stop() -> void {
            std::stop_source source;
            {
                std::lock_guard lock{ m_mutex };
                if (m_state != State::Running) { return; }
                m_state = State::Stopping;
                source = m_stop_source;
            }
            source.request_stop();
            std::lock_guard lock{ m_mutex };
            m_work.reset();
        }

        /**
         * @brief Wait for all owned and inline runners, then permit restart.
         * @return Success, Busy on any I/O thread, or Unknown if a callback failed.
         * @note Stop first; a running generation deliberately keeps its work guard alive.
         */
        auto Join() -> Result<void> {
            if (IsIoThread()) { return std::unexpected{ Error::Busy }; }
            std::lock_guard join_lock{ m_join_mutex };
            std::vector<std::thread> threads;
            {
                std::lock_guard lock{ m_mutex };
                if (m_state == State::Idle) {
                    return m_failed ? Result<void>{ std::unexpected{ Error::Unknown } } : Result<void>{};
                }
                threads.swap(m_threads);
            }
            for (auto& thread : threads) { thread.join(); }
            std::unique_lock lock{ m_mutex };
            m_drained.wait(lock, [this] { return m_runners == 0; });
            m_state = State::Idle;
            return m_failed ? Result<void>{ std::unexpected{ Error::Unknown } } : Result<void>{};
        }

        /**
         * @brief Observe whether a generation accepts new operations.
         * @return Whether Start or Run is active and Stop has not been requested.
         */
        [[nodiscard]] auto IsRunning() const -> bool {
            std::lock_guard lock{ m_mutex };
            return m_state == State::Running;
        }

        /**
         * @brief Detect any runtime I/O thread, including one executing an inline Run.
         * @return Whether blocking waits must be rejected on this thread.
         */
        [[nodiscard]] static auto IsIoThread() noexcept -> bool { return g_current_runtime != nullptr; }

        /**
         * @brief Borrow the context for internal executor and operation construction.
         * @return Context owned by the public Runtime for longer than every server/client using it.
         */
        [[nodiscard]] auto Context() noexcept -> asio::io_context& { return m_context; }

        /**
         * @brief Snapshot cancellation for the currently accepting generation without submitting work.
         * @return Stable generation token, or NotRunning for idle/stopping execution.
         */
        [[nodiscard]] auto StopToken() const -> Result<std::stop_token> {
            std::lock_guard lock{ m_mutex };
            if (m_state != State::Running) { return std::unexpected{ Error::NotRunning }; }
            return m_stop_source.get_token();
        }

        /**
         * @brief Lazily create a bounded pool for blocking application work.
         * @return Executor owned until this runtime's final destruction, after all roots drain.
         * @note Root work guards keep I/O runners alive while worker completions are outstanding.
         */
        [[nodiscard]] auto WorkerExecutor() -> asio::any_io_executor {
            std::lock_guard lock{ m_mutex };
            if (!m_workers) { m_workers = std::make_shared<asio::thread_pool>(2); }
            return m_workers->get_executor();
        }

        /**
         * @brief Submit initial work atomically with respect to stopping this generation.
         * @param operation Owned callback receiving this generation's cancellation token.
         * @return Success or NotRunning; no callback is queued on rejection.
         */
        auto Submit(std::function<void(std::stop_token)> operation) -> Result<void> {
            std::lock_guard lock{ m_mutex };
            if (m_state != State::Running) { return std::unexpected{ Error::NotRunning }; }
            asio::post(m_context, [operation = std::move(operation), token = m_stop_source.get_token()] {
                operation(token);
            });
            return {};
        }

    private:
        /**
         * @brief Prepare an idle generation while holding m_mutex and before creating runners.
         */
        auto prepare() -> void {
            m_context.restart();
            m_stop_source = std::stop_source{};
            m_work.emplace(asio::make_work_guard(m_context));
            m_failed = false;
            m_state = State::Running;
        }

        /**
         * @brief Execute callbacks, contain unexpected exceptions, and publish the drained state.
         */
        auto runLoop() noexcept -> Result<void> {
            g_current_runtime = this;
            for (;;) {
                try { m_context.run(); break; }
                catch (...) {
                    {
                        std::lock_guard lock{ m_mutex };
                        m_failed = true;
                    }
                    Stop();
                }
            }
            g_current_runtime = nullptr;
            Result<void> result;
            {
                std::lock_guard lock{ m_mutex };
                if (--m_runners == 0) { m_state = State::Drained; }
                if (m_failed) { result = std::unexpected{ Error::Unknown }; }
            }
            m_drained.notify_all();
            return result;
        }
    };

} // namespace mhttp::impl
