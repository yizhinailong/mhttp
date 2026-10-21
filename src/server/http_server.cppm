/**
 * @file http_server.cppm
 * @brief Expose an owned HTTP listener with bounded sessions and synchronous/asynchronous drain.
 */
export module mhttp:server.http_server;

import std;
import :server.impl.server;
export import :core.runtime;
export import :server.router;
export import :server.options;
export import :server.logging;

export namespace mhttp::server {

    /**
     * @brief Own HTTP routing, a listener, and strand-serialized sessions on a borrowed Runtime.
     * @note Runtime must outlive this object and every issued Task. Bind and blocking waits require
     *       non-I/O threads. Destroy on a non-I/O thread; destruction stops and drains active sessions.
     *       Stop preserves routes, permitting Bind/Start again after drain. Configuration is immutable
     *       between Start and completed stop; handlers on different sessions may execute concurrently.
     */
    class HttpServer : public Router {
    private:
        std::shared_ptr<impl::ServerImpl> m_impl; ///< Owned listener lifecycle and connection registry.

    public:
        /**
         * @brief Configure an idle listener without starting the borrowed runtime.
         * @param runtime Execution owner, alive until this server and its Tasks finish.
         * @param options Owned server limits and default response fields, validated at Bind.
         * @param router_options Route expression bounds, fixed at construction.
         */
        explicit HttpServer(
            Runtime& runtime,
            ServerOptions options = {},
            RouterOptions router_options = {}
        ) :
            Router{ router_options }, m_impl{ std::make_shared<impl::ServerImpl>(runtime, *this, std::move(options)) } {}

        /**
         * @brief Stop accepting and drain all callbacks before releasing routing and sockets.
         * @note Destruction on an I/O thread violates the blocking ownership contract and terminates.
         */
        ~HttpServer() override {
            if (Runtime::IsIoThread()) { std::terminate(); }
            static_cast<void>(m_impl->Stop());
        }

        /**
         * @brief Replace the diagnostic observer before startup, or clear it with an empty callback.
         * @param logger Owned synchronous callback receiving retainable failure metadata.
         * @return Success or AlreadyRunning until listener/session drain completes.
         * @note Calls run outside configuration locks and may overlap across sessions. Keep them short,
         *       synchronize shared state, and copy the record if retained. Exceptions never escape or
         *       recursively invoke the logger. Normal idle EOF and explicit cancellation are omitted.
         */
        [[nodiscard]] auto SetLogger(Logger logger) -> Result<void> {
            return m_impl->SetLogger(std::move(logger));
        }

        /**
         * @brief Replace the synchronous access observer before startup; an empty callback disables it.
         * @param logger Owned callback, including move-only captures, invoked after each complete final write.
         * @return Success or AlreadyRunning until stop drains all sessions and observers.
         * @note References borrow request/response state only during the callback. Calls across sessions
         *       can overlap; avoid blocking I/O execution and synchronize shared application state.
         *       Exceptions are contained and do not alter responses or connection persistence.
         */
        [[nodiscard]] auto SetAccessLogger(AccessLogger logger) -> Result<void> {
            return m_impl->SetAccessLogger(std::move(logger));
        }

        /**
         * @brief Bind and listen, deferring acceptance until Start.
         * @param host Numeric IP address or hostname; resolution can block this non-I/O caller.
         * @param port Zero requests dynamic port allocation.
         * @return Bound port or a bind/configuration/lifecycle failure.
         */
        [[nodiscard]] auto Bind(std::string const& host, std::uint16_t port) -> Result<std::uint16_t> {
            auto implementation = m_impl;
            return implementation->Bind(host, port);
        }

        /**
         * @brief Allocate a dynamic listening port before starting acceptance.
         * @param host Numeric IP address or hostname.
         * @return Nonzero bound port or failure.
         */
        [[nodiscard]] auto BindToAnyPort(std::string const& host) -> Result<std::uint16_t> { return Bind(host, 0); }

        /**
         * @brief Freeze routing and submit acceptance on an already running Runtime without blocking.
         * @return Success, AlreadyRunning, NotRunning, or Busy if standalone routing still owns configuration.
         */
        [[nodiscard]] auto Start() -> Result<void> {
            auto started = m_impl->Start();
            return started ? Result<void>{} : Result<void>{ std::unexpected{ started.error() } };
        }

        /**
         * @brief Bind/start and block until stop; run an idle Runtime on this calling thread.
         * @param host Numeric IP address or hostname.
         * @param port Desired listening port, or zero for dynamic allocation.
         * @return Startup/accept/runtime failure, Busy on an I/O thread, or success after drain.
         * @note With an idle Runtime, Listen owns Run/Join and stops that generation after server drain.
         *       An already started Runtime keeps running after Listen returns. Use another control
         *       thread to call Stop, or submit AsyncStop outside this server's own request handlers.
         */
        [[nodiscard]] auto Listen(std::string const& host, std::uint16_t port) -> Result<void> {
            auto implementation = m_impl;
            return implementation->Listen(host, port);
        }

        /**
         * @brief Observe whether acceptance has been submitted without a stop request.
         * @return Current lifecycle snapshot; WaitUntilReady also waits for initialization.
         */
        [[nodiscard]] auto IsRunning() const -> bool { return m_impl->IsRunning(); }

        /**
         * @brief Read owned address metadata without accessing the socket.
         * @return Bound endpoint, absent before binding or after stop completes.
         */
        [[nodiscard]] auto LocalEndpoint() const -> std::optional<Endpoint> { return m_impl->LocalEndpoint(); }

        /**
         * @brief Block a non-I/O caller until the submitted listener initializes, stops, or times out.
         * @param timeout Maximum wait, defaulting to five seconds.
         * @return Success, Busy, NotRunning, or ConnectionTimeout.
         */
        [[nodiscard]] auto WaitUntilReady(
            std::chrono::milliseconds timeout = std::chrono::seconds{ 5 }
        ) -> Result<void> {
            auto implementation = m_impl;
            return implementation->WaitUntilReady(timeout);
        }

        /**
         * @brief Stop from any non-I/O thread and wait until all accept/session callbacks drain.
         * @return Success, or Busy on an I/O thread; repeated calls are safe.
         */
        [[nodiscard]] auto Stop() -> Result<void> {
            auto implementation = m_impl;
            return implementation->Stop();
        }

        /**
         * @brief Own a lazy asynchronous stop-and-drain operation before initial suspension.
         * @return Task completing after all sessions and acceptance drain; never await from this server's handler.
         */
        [[nodiscard]] auto AsyncStop() -> Task<void> { return m_impl->AsyncStop(); }
    };

} // namespace mhttp::server
