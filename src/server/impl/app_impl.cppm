/**
 * @file app_impl.cppm
 * @brief Snapshot application settings and drive one blocking listener run with signal-triggered quit.
 */
module;

#include <csignal>

module mhttp:server.impl.app;

import std;
import asio;
import :core.runtime;
import :server.impl.server;

namespace mhttp::impl {

    /**
     * @brief Own application settings, the active listener, and quit requests shared across threads.
     * @note m_mutex protects every field. Run publishes its listener only after acceptance starts and
     *       Quit records a flag before reading it, so a quit racing startup is applied by whichever
     *       side observes the other. Settings are copied at Run; later changes apply to the next Run.
     */
    class AppImpl : public std::enable_shared_from_this<AppImpl> {
    private:
        using Clock = std::chrono::steady_clock; ///< Monotonic graceful shutdown budget.

        /**
         * @brief Hold the configuration one Run uses, isolated from later setter calls.
         */
        struct Settings {
            std::optional<Endpoint> listener; ///< Bind target, required by Run.
            std::size_t threads{ 1 }; ///< I/O thread count; zero selects hardware concurrency.
            server::ServerOptions options; ///< Listener limits, validated when binding.
            std::shared_ptr<server::Logger> logger; ///< Shared diagnostic callback, or empty.
            std::shared_ptr<server::AccessLogger> access_logger; ///< Shared access callback, or empty.
            Clock::duration shutdown_timeout{ std::chrono::seconds{ 5 } }; ///< Graceful budget used by Quit.
            bool handle_signals{ true }; ///< Whether Run turns SIGINT/SIGTERM into Quit.
        };

        mutable std::mutex m_mutex; ///< Settings, run state, and listener publication.
        std::condition_variable m_changed; ///< Wakes readiness and destruction waiters on publication changes.
        Settings m_settings; ///< Configuration for the next Run.
        std::shared_ptr<ServerImpl> m_server; ///< Listener published while its generation is active.
        Clock::duration m_active_timeout{}; ///< Graceful budget snapshot of the active Run.
        std::size_t m_completed_runs{}; ///< Completed Run calls, letting readiness waiters observe failed startups.
        bool m_running{}; ///< A Run call owns the runtime and listener.
        bool m_quit{}; ///< Quit was requested during the active Run.

    public:
        /**
         * @brief Replace the bind target used by the next Run.
         * @param host Numeric IP address or hostname, resolved by Run.
         * @param port Listening port; zero requests dynamic allocation.
         */
        auto SetListener(std::string host, std::uint16_t port) -> void {
            Endpoint listener{ std::move(host), port };
            std::lock_guard lock{ m_mutex };
            m_settings.listener = std::move(listener);
        }

        /**
         * @brief Replace the I/O thread count used by the next Run.
         * @param count Positive count, or zero for hardware concurrency.
         */
        auto SetThreadNum(std::size_t count) -> void {
            std::lock_guard lock{ m_mutex };
            m_settings.threads = count;
        }

        /**
         * @brief Replace listener limits used by the next Run.
         * @param options Owned options, validated when Run binds.
         */
        auto SetServerOptions(server::ServerOptions options) -> void {
            std::lock_guard lock{ m_mutex };
            m_settings.options = std::move(options);
        }

        /**
         * @brief Replace the diagnostic callback, retiring the old one outside the mutex.
         * @param logger Owned callback; empty disables diagnostics.
         */
        auto SetLogger(server::Logger logger) -> void {
            auto incoming = logger ? std::make_shared<server::Logger>(std::move(logger)) : nullptr;
            std::shared_ptr<server::Logger> retired;
            std::lock_guard lock{ m_mutex };
            retired = std::exchange(m_settings.logger, std::move(incoming));
        }

        /**
         * @brief Replace the access callback, retiring the old one outside the mutex.
         * @param logger Owned callback; empty disables access observation.
         */
        auto SetAccessLogger(server::AccessLogger logger) -> void {
            auto incoming = logger ? std::make_shared<server::AccessLogger>(std::move(logger)) : nullptr;
            std::shared_ptr<server::AccessLogger> retired;
            std::lock_guard lock{ m_mutex };
            retired = std::exchange(m_settings.access_logger, std::move(incoming));
        }

        /**
         * @brief Replace the graceful budget that Quit grants begun requests.
         * @param timeout Nonnegative budget, validated by Run.
         */
        auto SetShutdownTimeout(Clock::duration timeout) -> void {
            std::lock_guard lock{ m_mutex };
            m_settings.shutdown_timeout = timeout;
        }

        /**
         * @brief Leave SIGINT/SIGTERM to the application during later runs.
         */
        auto DisableSignalHandling() -> void {
            std::lock_guard lock{ m_mutex };
            m_settings.handle_signals = false;
        }

        /**
         * @brief Bind, start threads and acceptance, then block until quit, signal, or listener failure.
         * @param runtime Application-owned runtime, idle before and after this call.
         * @param router Application routes; frozen while the listener is active.
         * @return Success after natural drain, ShutdownTimeout after forced drain, or a startup failure.
         */
        auto Run(Runtime& runtime, server::Router const& router) -> Result<void> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            Settings settings;
            {
                std::lock_guard lock{ m_mutex };
                if (m_running) { return std::unexpected{ Error::AlreadyRunning }; }
                settings = m_settings;
                m_running = true;
                m_quit = false;
                m_active_timeout = settings.shutdown_timeout;
            }
            Result<void> result;
            try { result = serve(runtime, router, std::move(settings)); }
            catch (...) {
                finishRun();
                throw;
            }
            finishRun();
            return result;
        }

        /**
         * @brief Request graceful stop of the active Run without blocking; idle calls do nothing.
         * @note Safe from any thread, including request handlers and the signal watcher.
         */
        auto Quit() -> void {
            std::shared_ptr<ServerImpl> server;
            Clock::duration timeout{};
            {
                std::lock_guard lock{ m_mutex };
                if (!m_running) { return; }
                m_quit = true;
                server = m_server;
                timeout = m_active_timeout;
            }
            if (server) { static_cast<void>(server->RequestStop(timeout)); }
        }

        /**
         * @brief Quit and block a non-I/O caller until the active Run has returned.
         */
        auto QuitAndWait() -> void {
            Quit();
            std::unique_lock lock{ m_mutex };
            m_changed.wait(lock, [this] { return !m_running; });
        }

        /**
         * @brief Observe whether the active Run's listener accepts connections.
         * @return Whether a listener is started and has not received a stop request.
         */
        auto IsRunning() const -> bool {
            std::shared_ptr<ServerImpl> server;
            {
                std::lock_guard lock{ m_mutex };
                server = m_server;
            }
            return server && server->IsRunning();
        }

        /**
         * @brief Read the active listener's bound address.
         * @return Owned endpoint, absent outside an active Run.
         */
        auto LocalEndpoint() const -> std::optional<Endpoint> {
            std::shared_ptr<ServerImpl> server;
            {
                std::lock_guard lock{ m_mutex };
                server = m_server;
            }
            return server ? server->LocalEndpoint() : std::nullopt;
        }

        /**
         * @brief Block a non-I/O caller until the current or next Run accepts, fails, or the timeout expires.
         * @param timeout Maximum wait shared by publication and listener initialization.
         * @return Success, Busy, NotRunning after a Run ended first, or ConnectionTimeout.
         */
        auto WaitUntilReady(std::chrono::milliseconds timeout) -> Result<void> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            auto const started = Clock::now();
            std::shared_ptr<ServerImpl> server;
            {
                std::unique_lock lock{ m_mutex };
                auto const runs = m_completed_runs;
                if (!m_changed.wait_for(lock, timeout, [&] { return m_server || m_completed_runs != runs; })) {
                    return std::unexpected{ Error::ConnectionTimeout };
                }
                if (!m_server) { return std::unexpected{ Error::NotRunning }; }
                server = m_server;
            }
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
            return server->WaitUntilReady(std::max(timeout - elapsed, std::chrono::milliseconds::zero()));
        }

    private:
        /**
         * @brief Validate settings, bind, and own the runtime generation for one Run.
         * @param runtime Application-owned runtime, stopped and joined before returning.
         * @param router Application routes.
         * @param settings Owned snapshot for this Run.
         * @return Listener result, or the runtime's callback failure after an otherwise clean drain.
         */
        auto serve(
            Runtime& runtime,
            server::Router const& router,
            Settings settings
        ) -> Result<void> {
            if (!settings.listener || settings.shutdown_timeout < Clock::duration::zero()
                || settings.shutdown_timeout > Clock::duration::max() / 2) {
                return std::unexpected{ Error::InvalidRequest };
            }
            auto server = std::make_shared<ServerImpl>(runtime, router, std::move(settings.options));
            if (settings.logger) {
                static_cast<void>(server->SetLogger([logger = settings.logger](server::LogRecord const& record) {
                    (*logger)(record);
                }));
            }
            if (settings.access_logger) {
                static_cast<void>(server->SetAccessLogger([logger = settings.access_logger](
                    server::Request const& request, server::Response const& response, Clock::duration elapsed) {
                    (*logger)(request, response, elapsed);
                }));
            }
            if (auto bound = server->Bind(settings.listener->address, settings.listener->port); !bound) {
                return std::unexpected{ bound.error() };
            }
            auto const threads = settings.threads != 0 ? settings.threads
                : std::max<std::size_t>(std::thread::hardware_concurrency(), 1);
            if (auto started = runtime.Start(threads); !started) {
                static_cast<void>(server->Stop());
                return started;
            }
            std::shared_ptr<asio::signal_set> signals;
            Result<void> result;
            try {
                if (settings.handle_signals) { signals = watchSignals(runtime); }
                result = acceptUntilStopped(server, settings.shutdown_timeout);
            } catch (...) {
                static_cast<void>(server->Stop());
                unwatchSignals(signals);
                runtime.Stop();
                static_cast<void>(runtime.Join());
                throw;
            }
            unwatchSignals(signals);
            runtime.Stop();
            auto joined = runtime.Join();
            return result ? joined : result;
        }

        /**
         * @brief Start acceptance, publish the listener, and wait for its generation to drain.
         * @param server Bound listener on a running runtime.
         * @param timeout Graceful budget applied to a quit that arrived during startup.
         * @return Start/accept failure, forced shutdown cause, or success.
         */
        auto acceptUntilStopped(
            std::shared_ptr<ServerImpl> const& server,
            Clock::duration timeout
        ) -> Result<void> {
            auto generation = server->Start();
            if (!generation) {
                static_cast<void>(server->Stop());
                return std::unexpected{ generation.error() };
            }
            bool quit{};
            {
                std::lock_guard lock{ m_mutex };
                m_server = server;
                quit = m_quit;
            }
            m_changed.notify_all();
            if (quit) { static_cast<void>(server->RequestStop(timeout)); }
            return server->WaitUntilStopped(*generation);
        }

        /**
         * @brief Arm a strand-confined SIGINT/SIGTERM wait that requests Quit once.
         * @param runtime Running runtime whose context owns the wait.
         * @return Signal set retained by its pending wait until unwatchSignals cancels it.
         * @note A pending wait keeps the runtime's context busy, so every Run must cancel it before Join.
         */
        auto watchSignals(Runtime& runtime) -> std::shared_ptr<asio::signal_set> {
            auto signals = std::make_shared<asio::signal_set>(
                asio::make_strand(runtime.m_impl->Context()), SIGINT, SIGTERM);
            asio::post(signals->get_executor(), [signals, weak = weak_from_this()] {
                signals->async_wait([signals, weak](asio::error_code error, int) {
                    if (error) { return; }
                    if (auto self = weak.lock()) { self->Quit(); }
                });
            });
            return signals;
        }

        /**
         * @brief Cancel the pending wait and restore default signal dispositions on the signal strand.
         * @param signals Watched set, or empty when signal handling is disabled.
         */
        static auto unwatchSignals(std::shared_ptr<asio::signal_set> const& signals) -> void {
            if (!signals) { return; }
            asio::post(signals->get_executor(), [signals] {
                asio::error_code ignored;
                signals->cancel(ignored);
                signals->clear(ignored);
            });
        }

        /**
         * @brief Unpublish the listener and wake readiness and destruction waiters.
         */
        auto finishRun() -> void {
            std::shared_ptr<ServerImpl> retired;
            {
                std::lock_guard lock{ m_mutex };
                retired = std::move(m_server);
                m_running = false;
                m_quit = false;
                ++m_completed_runs;
            }
            m_changed.notify_all();
        }
    };

} // namespace mhttp::impl
