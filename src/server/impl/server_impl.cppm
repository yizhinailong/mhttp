/**
 * @file server_impl.cppm
 * @brief Serialize accepting, session registration, cancellation, and server generation drain.
 */
module mhttp:server.impl.server;

import std;
import asio;
import :core.impl.io_error;
import :core.runtime;
import :server.impl.session;

namespace mhttp::impl {

    /**
     * @brief Own server lifecycle while keeping socket/table mutation on a server strand.
     * @note The API mutex protects lifecycle publication and bound sockets before submission.
     *       Once submitted, the server strand alone owns acceptor and session-table access.
     *       Stop never holds the API mutex while waiting for asynchronous operations to drain.
     */
    class ServerImpl : public std::enable_shared_from_this<ServerImpl> {
    private:
        /**
         * @brief Distinguish a bound idle socket from a submitted or draining accept loop.
         */
        enum class State { Idle, Bound, Running, Stopping };

        /**
         * @brief Own one immutable runtime generation and all sessions registered before spawning.
         */
        struct Generation {
            std::shared_ptr<asio::ip::tcp::acceptor> acceptor; ///< Server-strand owned after submission.
            std::unordered_map<std::size_t, std::shared_ptr<Session>> sessions; ///< Server-strand registry.
            std::vector<std::shared_ptr<asio::steady_timer>> waiters; ///< AsyncStop drain notifications.
            std::unique_ptr<std::stop_callback<std::function<void()>>> runtime_stop; ///< Owned cancellation link.
            std::stop_token token; ///< Runtime generation cancellation for each accepted session.
            bool stop_runtime{}; ///< Immutable: Listen owns this inline runtime generation.
            std::size_t next_id{}; ///< Server-strand session identity.
            bool stopping{}; ///< Server-strand stop guard.
            bool accept_done{}; ///< Accept root and its frame have drained.
            bool finished{}; ///< Server-strand final publication guard.
            bool ready{}; ///< Published under the API mutex.
            bool completed{}; ///< Published under the API mutex after all roots drain.
            std::optional<Error> error; ///< Published terminal accept/start failure under API mutex.
        };

        std::shared_ptr<RuntimeImpl> m_runtime; ///< Context owner; public Runtime must still outlive the server.
        std::shared_ptr<RouterImpl> m_router; ///< Stable route identity retained independently of public wrappers.
        server::ServerOptions m_options; ///< Immutable configuration snapshot, validated at Bind.
        asio::any_io_executor m_executor; ///< Server strand, independent of every session strand.
        mutable std::mutex m_mutex; ///< API state and readiness/completion publication.
        std::condition_variable m_changed; ///< Non-I/O readiness and Stop waiters.
        State m_state{ State::Idle }; ///< API state; protected by m_mutex.
        std::shared_ptr<asio::ip::tcp::acceptor> m_bound; ///< Mutex-owned socket with no pending operations.
        std::shared_ptr<Generation> m_active; ///< Published generation; retained by all asynchronous roots.
        std::optional<Endpoint> m_local; ///< Bound endpoint snapshot; protected by m_mutex.

    public:
        /**
         * @brief Retain runtime and routing state without creating sockets or starting execution.
         * @param runtime Borrowed only for extracting ownership; its public owner must outlive this server.
         * @param router Borrowed only while extracting its owned configuration.
         * @param options Owned resource limits, validated when binding.
         */
        ServerImpl(Runtime const& runtime, server::Router const& router, server::ServerOptions options) :
            m_runtime{ runtime.m_impl }, m_router{ router.m_impl }, m_options{ std::move(options) },
            m_executor{ asio::make_strand(m_runtime->Context()) } {}

        /**
         * @brief Bind/listen on a non-I/O caller before transferring the socket to the server strand.
         * @param host Owned numeric address or hostname; DNS resolution may block this caller.
         * @param port Zero requests a dynamic port.
         * @return Bound port, Busy on an I/O thread, AlreadyRunning, or a configuration/bind failure.
         */
        auto Bind(std::string const& host, std::uint16_t port) -> Result<std::uint16_t> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            std::lock_guard lock{ m_mutex };
            if (m_state == State::Running || m_state == State::Stopping) {
                return std::unexpected{ Error::AlreadyRunning };
            }
            if (auto valid = detail::validate_server_options(m_options); !valid) {
                return std::unexpected{ valid.error() };
            }
            asio::error_code error;
            asio::ip::tcp::resolver resolver{ m_executor };
            auto endpoints = resolver.resolve(host, std::to_string(port), error);
            if (error || endpoints.empty()) { return std::unexpected{ Error::BindIpAddress }; }
            for (auto const& entry : endpoints) {
                auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(m_executor);
                acceptor->open(entry.endpoint().protocol(), error);
                if (error) { continue; }
                acceptor->bind(entry.endpoint(), error);
                if (error) { continue; }
                acceptor->listen(asio::socket_base::max_listen_connections, error);
                if (error) { continue; }
                auto local = acceptor->local_endpoint(error);
                if (error) { continue; }
                m_bound = std::move(acceptor);
                m_local = Endpoint{ local.address().to_string(), local.port() };
                m_state = State::Bound;
                return local.port();
            }
            return std::unexpected{ Error::BindIpAddress };
        }

        /**
         * @brief Freeze routing and submit an owned accept generation without waiting.
         * @param stop_runtime Whether this generation's drain also stops a Listen-owned inline Runtime.
         * @return Owned submitted generation, AlreadyRunning, NotRunning, or Busy routing.
         */
        auto Start(bool stop_runtime = false) -> Result<std::shared_ptr<Generation>> {
            std::lock_guard lock{ m_mutex };
            if (m_state == State::Running || m_state == State::Stopping) {
                return std::unexpected{ Error::AlreadyRunning };
            }
            if (m_state != State::Bound) { return std::unexpected{ Error::NotRunning }; }
            auto generation = std::make_shared<Generation>();
            generation->acceptor = m_bound;
            generation->stop_runtime = stop_runtime;
            auto operation = [self = shared_from_this(), generation](std::stop_token token) {
                asio::post(self->m_executor, [self, generation, token] {
                    try { self->startGeneration(generation, token); }
                    catch (...) {
                        generation->accept_done = true;
                        {
                            std::lock_guard lock{ self->m_mutex };
                            generation->error = Error::Unknown;
                        }
                        self->stopGeneration(generation);
                    }
                });
            };
            if (auto frozen = m_router->Freeze(); !frozen) { return std::unexpected{ frozen.error() }; }
            Result<void> submitted;
            try { submitted = m_runtime->Submit(std::move(operation)); }
            catch (...) {
                static_cast<void>(m_router->Thaw());
                throw;
            }
            if (!submitted) {
                static_cast<void>(m_router->Thaw());
                return std::unexpected{ submitted.error() };
            }
            m_active = generation;
            m_bound.reset();
            m_state = State::Running;
            return generation;
        }

        /**
         * @brief Bind/start and block this caller, running an idle Runtime inline until server drain.
         * @param host Owned numeric address or hostname.
         * @param port Zero requests a dynamic port, observable through LocalEndpoint after startup.
         * @return Startup/accept/runtime failure, or success after complete listener drain.
         * @note An already running Runtime keeps its execution mode and continues after this server stops.
         */
        auto Listen(std::string const& host, std::uint16_t port) -> Result<void> {
            if (auto bound = Bind(host, port); !bound) { return std::unexpected{ bound.error() }; }
            if (m_runtime->IsRunning()) {
                auto started = Start();
                if (!started) { static_cast<void>(Stop()); return std::unexpected{ started.error() }; }
                std::unique_lock lock{ m_mutex };
                m_changed.wait(lock, [&] { return (*started)->completed; });
                return (*started)->error ? Result<void>{ std::unexpected{ *(*started)->error } } : Result<void>{};
            }
            Result<std::shared_ptr<Generation>> started{ std::unexpected{ Error::NotRunning } };
            auto run = m_runtime->Run([self = shared_from_this(), &started](std::stop_token) {
                try { started = self->Start(true); }
                catch (...) {
                    self->requestStop();
                    self->m_runtime->Stop();
                    throw;
                }
                if (!started) {
                    self->requestStop();
                    self->m_runtime->Stop();
                }
            });
            if (!run) {
                static_cast<void>(Stop());
                if (run.error() == Error::Unknown) { static_cast<void>(m_runtime->Join()); }
                return run;
            }
            if (auto joined = m_runtime->Join(); !joined) { return joined; }
            if (!started) { return std::unexpected{ started.error() }; }
            std::lock_guard lock{ m_mutex };
            return (*started)->error ? Result<void>{ std::unexpected{ *(*started)->error } } : Result<void>{};
        }

        /**
         * @brief Observe whether a generation is submitted and has not received a stop request.
         * @return API lifecycle snapshot.
         */
        auto IsRunning() const -> bool {
            std::lock_guard lock{ m_mutex };
            return m_state == State::Running;
        }

        /**
         * @brief Borrow no socket state while observing a bound endpoint.
         * @return Owned snapshot, absent before binding and after completed stop.
         */
        auto LocalEndpoint() const -> std::optional<Endpoint> {
            std::lock_guard lock{ m_mutex };
            return m_local;
        }

        /**
         * @brief Wait for one submitted generation to initialize, or for stop/failure/timeout.
         * @param timeout Maximum caller wait; nonpositive values perform an immediate check.
         * @return Success, Busy on I/O threads, NotRunning, or ConnectionTimeout.
         */
        auto WaitUntilReady(std::chrono::milliseconds timeout) -> Result<void> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            std::unique_lock lock{ m_mutex };
            auto generation = m_active;
            if (!generation || m_state != State::Running) { return std::unexpected{ Error::NotRunning }; }
            if (!m_changed.wait_for(lock, timeout, [&] {
                return generation->ready || generation->completed || m_active != generation || m_state != State::Running;
            })) { return std::unexpected{ Error::ConnectionTimeout }; }
            if (generation->error) { return std::unexpected{ *generation->error }; }
            return generation->ready && m_active == generation && m_state == State::Running
                ? Result<void>{} : Result<void>{ std::unexpected{ Error::NotRunning } };
        }

        /**
         * @brief Close bound/active sockets and block only a non-I/O caller until all session roots drain.
         * @return Success, or Busy without initiating a blocking wait on an I/O thread.
         */
        auto Stop() -> Result<void> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            auto generation = requestStop();
            if (generation) {
                std::unique_lock lock{ m_mutex };
                m_changed.wait(lock, [&] { return generation->completed; });
            }
            return {};
        }

        /**
         * @brief Own the server before publishing a lazy drain Task.
         * @return Task completing after the accept loop, callbacks, and session registry drain.
         * @note Await outside this server's own handlers: a handler is itself part of the drain.
         */
        auto AsyncStop() -> Task<void> { return ToTask(m_executor, stopAndWait(shared_from_this())); }

    private:
        /**
         * @brief Publish stopping before posting strand-owned cancellation; close unsubmitted sockets directly.
         * @return Owned active generation, or empty if no asynchronous drain is needed.
         */
        auto requestStop() -> std::shared_ptr<Generation> {
            std::shared_ptr<Generation> generation;
            {
                std::lock_guard lock{ m_mutex };
                if (m_state == State::Idle) { return {}; }
                if (m_state == State::Bound) {
                    m_bound.reset();
                    m_local.reset();
                    m_state = State::Idle;
                } else {
                    generation = m_active;
                    m_state = State::Stopping;
                }
            }
            m_changed.notify_all();
            if (generation) {
                asio::post(m_executor, [self = shared_from_this(), generation] { self->stopGeneration(generation); });
            }
            return generation;
        }

        /**
         * @brief Link runtime cancellation before starting the accept root, including an already-stopped token.
         * @param generation Owned submitted state.
         * @param token Immutable runtime generation token.
         */
        auto startGeneration(std::shared_ptr<Generation> generation, std::stop_token token) -> void {
            generation->token = token;
            generation->runtime_stop = std::make_unique<std::stop_callback<std::function<void()>>>(token,
                [weak = weak_from_this(), generation = std::weak_ptr{ generation }] {
                    if (auto self = weak.lock()) {
                        if (auto active = generation.lock()) {
                            asio::post(self->m_executor, [self, active] { self->stopGeneration(active); });
                        }
                    }
                });
            asio::co_spawn(m_executor, acceptLoop(shared_from_this(), generation),
                [self = shared_from_this(), generation](std::exception_ptr failure, Result<void> result) {
                    generation->accept_done = true;
                    if (failure || !result) {
                        std::lock_guard lock{ self->m_mutex };
                        generation->error = failure ? Error::Unknown : result.error();
                    }
                    self->stopGeneration(generation);
                });
            {
                std::lock_guard lock{ m_mutex };
                if (m_state == State::Running && !token.stop_requested()) { generation->ready = true; }
            }
            m_changed.notify_all();
        }

        /**
         * @brief Accept and register each session before spawning its owned connection-strand root.
         * @param self Owned server through accept-root completion.
         * @param generation Owned registry and acceptor, accessed only on the server strand.
         * @return Fatal accept failure, or success once canceled for stop.
         */
        static auto acceptLoop(
            std::shared_ptr<ServerImpl> self,
            std::shared_ptr<Generation> generation
        ) -> asio::awaitable<Result<void>> {
            while (!generation->stopping) {
                auto connection = Connection::Create(self->m_runtime->Context().get_executor(),
                    { .max_write_size = self->m_options.max_response_bytes });
                if (!connection) { co_return std::unexpected{ connection.error() }; }
                auto [error] = co_await generation->acceptor->async_accept((*connection)->SocketForInitialization(),
                    asio::as_tuple(asio::use_awaitable));
                if (generation->stopping) { (*connection)->Close(); break; }
                if (error) { co_return std::unexpected{ Error::Connection }; }
                auto session = Session::Create(*connection, self->m_router, self->m_options,
                    self->m_runtime, generation->token);
                if (!session) { (*connection)->Close(); continue; }
                auto const id = generation->next_id++;
                generation->sessions.emplace(id, *session);
                try {
                    asio::co_spawn(self->m_executor, (*session)->Run(),
                        [self, generation, id, owner = *session](std::exception_ptr failure, SessionResult) {
                            if (failure) { owner->Stop(); }
                            generation->sessions.erase(id);
                            self->finishGeneration(generation);
                        });
                } catch (...) {
                    generation->sessions.erase(id);
                    (*connection)->Close();
                    throw;
                }
            }
            co_return Result<void>{};
        }

        /**
         * @brief Cancel accept and every registered connection before checking the drain predicate.
         * @param generation Owned strand-confined state; repeated or late calls are harmless.
         */
        auto stopGeneration(std::shared_ptr<Generation> const& generation) -> void {
            if (generation->finished) { return; }
            if (!generation->stopping) {
                generation->stopping = true;
                {
                    std::lock_guard lock{ m_mutex };
                    if (m_active == generation) { m_state = State::Stopping; }
                }
                asio::error_code ignored;
                generation->acceptor->close(ignored);
                for (auto const& [id, session] : generation->sessions) { session->Stop(); }
                m_changed.notify_all();
            }
            finishGeneration(generation);
        }

        /**
         * @brief Thaw routes only after both accept and every session root have completed.
         * @param generation Owned server-strand state.
         */
        auto finishGeneration(std::shared_ptr<Generation> const& generation) -> void {
            if (!generation->stopping || !generation->accept_done || !generation->sessions.empty()
                || generation->finished) { return; }
            generation->finished = true;
            generation->runtime_stop.reset();
            static_cast<void>(m_router->Thaw());
            if (generation->stop_runtime) { m_runtime->Stop(); }
            {
                std::lock_guard lock{ m_mutex };
                generation->completed = true;
                if (m_active == generation) {
                    m_active.reset();
                    m_local.reset();
                    m_state = State::Idle;
                }
            }
            for (auto const& waiter : generation->waiters) { waiter->cancel(); }
            generation->waiters.clear();
            m_changed.notify_all();
        }

        /**
         * @brief Ignore caller cancellation until server drain completes, then let the Task bridge report it.
         * @param self Owned server, retained before initial suspension.
         */
        static auto stopAndWait(std::shared_ptr<ServerImpl> self) -> asio::awaitable<void> {
            co_await asio::this_coro::reset_cancellation_state([](asio::cancellation_type) {
                return asio::cancellation_type::none;
            });
            auto generation = self->requestStop();
            if (!generation || generation->finished) { co_return; }
            auto waiter = std::make_shared<asio::steady_timer>(self->m_executor,
                std::chrono::steady_clock::time_point::max());
            generation->waiters.push_back(waiter);
            static_cast<void>(co_await waiter->async_wait(asio::as_tuple(asio::use_awaitable)));
        }
    };

} // namespace mhttp::impl
