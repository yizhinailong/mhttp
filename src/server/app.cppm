/**
 * @file app.cppm
 * @brief Expose a drogon-style process-wide application owning routes, one listener, and its I/O threads.
 */
export module mhttp:server.app;

import std;
import :server.impl.app;
export import :core.endpoint;
export import :core.runtime;
export import :server.router;
export import :server.options;
export import :server.logging;

export namespace mhttp {

    /**
     * @brief Own routes, one listener, and I/O threads behind chainable setters and a blocking Run.
     * @note Register routes through the inherited set_http_handler templates. Setters may be called from
     *       any thread; Run copies them, so changes made while running apply to the next Run. Run blocks
     *       a non-I/O thread until Quit, SIGINT/SIGTERM, or listener failure, and may be called again
     *       afterwards. Destroy on a non-I/O thread; destruction quits an active Run and waits for it.
     */
    class App : public server::Router {
    private:
        Runtime m_runtime; ///< I/O threads started and joined by each Run.
        std::shared_ptr<impl::AppImpl> m_impl; ///< Settings, active listener, and quit state.

    public:
        /**
         * @brief Create an idle application without binding or starting threads.
         * @param router_options Route expression bounds, fixed at construction.
         */
        explicit App(server::RouterOptions router_options = {}) :
            Router{ router_options }, m_impl{ std::make_shared<impl::AppImpl>() } {}

        /**
         * @brief Quit an active Run and wait for it to return before releasing routes and threads.
         * @note Destruction on an I/O thread would wait on itself and terminates instead.
         */
        ~App() override {
            if (Runtime::IsIoThread()) { std::terminate(); }
            m_impl->QuitAndWait();
        }

        /**
         * @brief Select the address the next Run binds; a later call replaces it.
         * @param host Numeric IP address or hostname; Run resolves it on the calling thread.
         * @param port Listening port; zero requests dynamic allocation, observable through LocalEndpoint.
         * @return This application for chaining.
         */
        auto SetListener(std::string host, std::uint16_t port) -> App& {
            m_impl->SetListener(std::move(host), port);
            return *this;
        }

        /**
         * @brief Select how many I/O threads the next Run starts; the default is one.
         * @param count Thread count; zero selects std::thread::hardware_concurrency(), or one if unknown.
         * @return This application for chaining.
         */
        auto SetThreadNum(std::size_t count) -> App& {
            m_impl->SetThreadNum(count);
            return *this;
        }

        /**
         * @brief Replace listener limits, timeouts, and default response fields for the next Run.
         * @param options Owned options; Run returns InvalidRequest for invalid limits.
         * @return This application for chaining.
         */
        auto SetServerOptions(server::ServerOptions options) -> App& {
            m_impl->SetServerOptions(std::move(options));
            return *this;
        }

        /**
         * @brief Replace the diagnostic observer for the next Run, or clear it with an empty callback.
         * @param logger Owned callback with the same contract as HttpServer::SetLogger.
         * @return This application for chaining.
         */
        auto SetLogger(server::Logger logger) -> App& {
            m_impl->SetLogger(std::move(logger));
            return *this;
        }

        /**
         * @brief Replace the access observer for the next Run, or clear it with an empty callback.
         * @param logger Owned callback with the same contract as HttpServer::SetAccessLogger.
         * @return This application for chaining.
         */
        auto SetAccessLogger(server::AccessLogger logger) -> App& {
            m_impl->SetAccessLogger(std::move(logger));
            return *this;
        }

        /**
         * @brief Select how long Quit lets begun requests finish before canceling them; the default is five seconds.
         * @param timeout Nonnegative budget; Run returns InvalidRequest for negative or excessive values.
         * @return This application for chaining.
         */
        auto SetShutdownTimeout(std::chrono::steady_clock::duration timeout) -> App& {
            m_impl->SetShutdownTimeout(timeout);
            return *this;
        }

        /**
         * @brief Stop later runs from turning SIGINT/SIGTERM into Quit, leaving signals to the application.
         * @return This application for chaining.
         */
        auto DisableSignalHandling() -> App& {
            m_impl->DisableSignalHandling();
            return *this;
        }

        /**
         * @brief Bind, start the I/O threads and listener, and block until the application stops.
         * @return Success after natural drain; ShutdownTimeout when Quit's budget expired and begun requests
         *         were canceled; InvalidRequest without a listener or with invalid settings; bind, accept,
         *         or runtime failures; AlreadyRunning during another Run; or Busy on an I/O thread.
         * @note Routes are frozen while running. Unless disabled, SIGINT/SIGTERM call Quit and default
         *       signal dispositions are restored before returning. Do not Start or Stop GetRuntime() directly.
         */
        [[nodiscard]] auto Run() -> Result<void> {
            auto implementation = m_impl;
            return implementation->Run(m_runtime, *this);
        }

        /**
         * @brief Ask the active Run to stop gracefully and return without waiting; idle calls do nothing.
         * @note Safe from any thread, including this application's own request handlers.
         */
        auto Quit() -> void { m_impl->Quit(); }

        /**
         * @brief Observe whether the active Run's listener accepts connections.
         * @return Whether a listener is started and has not received a stop request.
         */
        [[nodiscard]] auto IsRunning() const -> bool { return m_impl->IsRunning(); }

        /**
         * @brief Read the bound address of the active Run.
         * @return Owned endpoint, absent before the listener starts and after Run returns.
         */
        [[nodiscard]] auto LocalEndpoint() const -> std::optional<Endpoint> { return m_impl->LocalEndpoint(); }

        /**
         * @brief Block a non-I/O caller until the current or next Run accepts connections.
         * @param timeout Maximum wait, defaulting to five seconds.
         * @return Success, NotRunning if a Run ended before becoming ready, ConnectionTimeout, or Busy.
         */
        [[nodiscard]] auto WaitUntilReady(
            std::chrono::milliseconds timeout = std::chrono::seconds{ 5 }
        ) -> Result<void> {
            auto implementation = m_impl;
            return implementation->WaitUntilReady(timeout);
        }

        /**
         * @brief Borrow the runtime that Run starts, for clients, SyncWait, and Offload during a run.
         * @return Runtime owned by this application and running only while Run is active.
         */
        [[nodiscard]] auto GetRuntime() noexcept -> Runtime& { return m_runtime; }
    };

    /**
     * @brief Access the process-wide application, constructed on first use.
     * @return Application destroyed during static destruction; let Run return before main exits.
     */
    [[nodiscard]] auto app() -> App& {
        static App instance;
        return instance;
    }

} // namespace mhttp
