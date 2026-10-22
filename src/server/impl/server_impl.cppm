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
import :server.impl.socket_options;

namespace mhttp::impl {

    /**
     * @brief Own server lifecycle while keeping socket/table mutation on a server strand.
     * @note The API mutex protects lifecycle publication and bound sockets before submission.
     *       Once submitted, the server strand alone owns acceptor and session-table access.
     *       Stop never holds the API mutex while waiting for asynchronous operations to drain.
     */
    class ServerImpl : public std::enable_shared_from_this<ServerImpl> {
    private:
        using Clock = std::chrono::steady_clock; ///< Monotonic graceful shutdown budget.
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
            std::shared_ptr<std::atomic<bool>> admission_closed{ std::make_shared<std::atomic<bool>>() }; ///< New-request fence.
            std::shared_ptr<asio::steady_timer> shutdown_timer; ///< Optional strand-owned graceful deadline.
            std::optional<Clock::time_point> shutdown_deadline; ///< Earliest requested graceful deadline.
            std::size_t drain_commands{}; ///< Session drain commands whose completions must be joined.
            bool timer_pending{}; ///< Deadline callback still owns generation state.
            bool aborting{}; ///< Immediate cancellation has superseded graceful completion.
            std::stop_token token; ///< Runtime generation cancellation for each accepted session.
            ServerObservers observers; ///< Owned callback snapshot, immutable after submission.
            Endpoint local; ///< Immutable bound address snapshot for diagnostics.
            asio::error_code accept_error; ///< Server-strand native accept failure, if any.
            bool stop_runtime{}; ///< Immutable: Listen owns this inline runtime generation.
            std::size_t next_id{}; ///< Server-strand session identity.
            bool stopping{}; ///< Server-strand stop guard.
            bool accept_done{}; ///< Accept root and its frame have drained.
            bool finished{}; ///< Server-strand final publication guard.
            bool ready{}; ///< Published under the API mutex.
            bool completed{}; ///< Published under the API mutex after all roots drain.
            std::optional<Error> error; ///< Published terminal accept/start failure under API mutex.
            std::optional<Error> shutdown_error; ///< Forced drain cause, published under the API mutex.
        };

        std::shared_ptr<RuntimeImpl> m_runtime; ///< Context owner; public Runtime must still outlive the server.
        std::shared_ptr<RouterImpl> m_router; ///< Stable route identity retained independently of public wrappers.
        server::ServerOptions m_options; ///< Immutable configuration snapshot, validated at Bind.
        ServerObservers m_observers; ///< Callback identities, replaced only while idle/bound under m_mutex.
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
         * @brief Replace diagnostic callback ownership while no generation is active.
         * @param logger Owned callback; empty disables diagnostics.
         * @return Success or AlreadyRunning, retiring callback resources outside the API mutex.
         */
        auto SetLogger(server::Logger logger) -> Result<void> {
            auto incoming = logger ? std::make_shared<server::Logger>(std::move(logger)) : nullptr;
            std::shared_ptr<server::Logger> retired;
            std::lock_guard lock{ m_mutex };
            if (m_state == State::Running || m_state == State::Stopping) {
                return std::unexpected{ Error::AlreadyRunning };
            }
            retired = std::exchange(m_observers.logger, std::move(incoming));
            return {};
        }

        /**
         * @brief Replace the owned access callback before startup, retiring old state outside the API mutex.
         * @param logger Owned synchronous callback; empty disables observation.
         * @return Success, or AlreadyRunning until the active generation drains.
         */
        auto SetAccessLogger(server::AccessLogger logger) -> Result<void> {
            auto incoming = logger ? std::make_shared<server::AccessLogger>(std::move(logger)) : nullptr;
            std::shared_ptr<server::AccessLogger> retired;
            std::lock_guard lock{ m_mutex };
            if (m_state == State::Running || m_state == State::Stopping) {
                return std::unexpected{ Error::AlreadyRunning };
            }
            retired = std::exchange(m_observers.access_logger, std::move(incoming));
            return {};
        }

        /**
         * @brief Bind/listen on a non-I/O caller before transferring the socket to the server strand.
         * @param host Owned numeric address or hostname; DNS resolution may block this caller.
         * @param port Zero requests a dynamic port.
         * @return Bound port, Busy on an I/O thread, AlreadyRunning, or a configuration/bind failure.
         */
        auto Bind(std::string const& host, std::uint16_t port) -> Result<std::uint16_t> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            asio::error_code error;
            Result<std::uint16_t> result;
            ServerObservers observers;
            {
                std::lock_guard lock{ m_mutex };
                if (m_state == State::Running || m_state == State::Stopping) {
                    return std::unexpected{ Error::AlreadyRunning };
                }
                observers = m_observers;
                result = bind(host, port, error);
            }
            if (!result) {
                try {
                    Endpoint target{ host, port };
                    observers.Log(server::LogEvent::BindFailure, result.error(), error, {}, nullptr, &target);
                } catch (...) {} // Diagnostic allocation must not change the bind result.
            }
            return result;
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
            generation->observers = m_observers;
            generation->local = *m_local;
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
                        generation->observers.Log(server::LogEvent::AcceptFailure, Error::Unknown, {},
                            std::current_exception(), nullptr, &generation->local);
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
         * @brief Stop admission, allow begun requests to finish, then cancel survivors at the deadline.
         * @param grace Nonnegative budget beginning at this call; excessive durations are rejected.
         * @return Success after natural drain, ShutdownTimeout after deadline cancellation, or lifecycle failure.
         * @note Cleanup still waits for executing Offload functions; a deadline initiates cancellation.
         */
        auto Stop(Clock::duration grace) -> Result<void> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            auto deadline = shutdownDeadline(grace);
            if (!deadline) { return std::unexpected{ deadline.error() }; }
            auto generation = requestStop(*deadline);
            if (generation) {
                std::unique_lock lock{ m_mutex };
                m_changed.wait(lock, [&] { return generation->completed; });
            }
            return shutdownResult(generation);
        }

        /**
         * @brief Own the server before publishing a lazy drain Task.
         * @return Task completing after the accept loop, callbacks, and session registry drain.
         * @note Await outside this server's own handlers: a handler is itself part of the drain.
         */
        auto AsyncStop() -> Task<void> { return stopTask(shared_from_this()); }

        /**
         * @brief Own a graceful stop operation before initial suspension.
         * @param grace Nonnegative budget measured when the lazy Task starts.
         * @return Cancellation-aware complete drain result on the caller's scheduler.
         */
        auto AsyncStop(Clock::duration grace) -> Task<Result<void>> { return gracefulStopTask(shared_from_this(), grace); }

        /**
         * @brief Close admission and schedule drain without waiting, so I/O threads may call it.
         * @param grace Graceful budget beginning now, or absent to cancel every session immediately.
         * @return Success, or InvalidRequest for an invalid grace before changing lifecycle state.
         */
        auto RequestStop(std::optional<Clock::duration> grace) -> Result<void> {
            std::optional<Clock::time_point> deadline;
            if (grace) {
                auto valid = shutdownDeadline(*grace);
                if (!valid) { return std::unexpected{ valid.error() }; }
                deadline = *valid;
            }
            static_cast<void>(requestStop(deadline));
            return {};
        }

        /**
         * @brief Block a non-I/O caller until one started generation stops on request or by failure.
         * @param generation Owned generation returned by Start.
         * @return Start/accept failure, forced shutdown cause, or success after natural drain.
         */
        auto WaitUntilStopped(std::shared_ptr<Generation> const& generation) -> Result<void> {
            if (RuntimeImpl::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            {
                std::unique_lock lock{ m_mutex };
                m_changed.wait(lock, [&] { return generation->completed; });
            }
            return shutdownResult(generation);
        }

    private:
        /**
         * @brief Validate and own an absolute shutdown deadline without mutating lifecycle state.
         * @param grace Nonnegative, representable relative budget.
         * @return Absolute monotonic deadline or InvalidRequest.
         */
        static auto shutdownDeadline(Clock::duration grace) -> Result<Clock::time_point> {
            auto const now = Clock::now();
            if (grace < Clock::duration::zero() || grace > Clock::duration::max() / 2
                || now.time_since_epoch() > Clock::duration::max() - grace) {
                return std::unexpected{ Error::InvalidRequest };
            }
            return now + grace;
        }

        /**
         * @brief Read one completed generation's result without inspecting a later restarted listener.
         * @param generation Owned completed generation, or absent for an idle/bound-only stop.
         * @return Natural completion or the first terminal accept/forced shutdown cause.
         */
        auto shutdownResult(std::shared_ptr<Generation> const& generation) const -> Result<void> {
            if (!generation) { return {}; }
            std::lock_guard lock{ m_mutex };
            auto const error = generation->error ? generation->error : generation->shutdown_error;
            return error ? Result<void>{ std::unexpected{ *error } } : Result<void>{};
        }
        /**
         * @brief Configure and bind a new socket while the API mutex excludes submission and replacement.
         * @param host Requested bind hostname or numeric address.
         * @param port Requested port, possibly zero.
         * @param error Native failure retained for diagnostics, clear for configuration rejection.
         * @return Bound port or failure, preserving any previous bound socket on rejection.
         */
        auto bind(
            std::string const& host,
            std::uint16_t port,
            asio::error_code& error
        ) -> Result<std::uint16_t> {
            if (auto valid = detail::validate_server_options(m_options); !valid) {
                return std::unexpected{ valid.error() };
            }
            asio::ip::tcp::resolver resolver{ m_executor };
            auto endpoints = resolver.resolve(host, std::to_string(port), error);
            if (error || endpoints.empty()) { return std::unexpected{ Error::BindIpAddress }; }
            for (auto const& entry : endpoints) {
                auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(m_executor);
                acceptor->open(entry.endpoint().protocol(), error);
                if (error) { continue; }
                error = configure_listener_socket(*acceptor, entry.endpoint().protocol(), m_options);
                if (error) { continue; }
                acceptor->bind(entry.endpoint(), error);
                if (error) { continue; }
                acceptor->listen(asio::socket_base::max_listen_connections, error);
                if (error) { continue; }
                auto local = acceptor->local_endpoint(error);
                if (error) { continue; }
                Endpoint snapshot{ local.address().to_string(), local.port() };
                m_bound = std::move(acceptor);
                m_local = std::move(snapshot);
                m_state = State::Bound;
                return local.port();
            }
            return std::unexpected{ Error::BindIpAddress };
        }

        /**
         * @brief Publish stopping before posting strand-owned cancellation; close unsubmitted sockets directly.
         * @param deadline Graceful cutoff, or absent to cancel all sessions immediately.
         * @return Owned active generation, or empty if no asynchronous drain is needed.
         */
        auto requestStop(std::optional<Clock::time_point> deadline = {}) -> std::shared_ptr<Generation> {
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
                    generation->admission_closed->store(true);
                    m_state = State::Stopping;
                }
            }
            m_changed.notify_all();
            if (generation) {
                asio::post(m_executor, [self = shared_from_this(), generation, deadline] {
                    try {
                        if (deadline) { self->drainGeneration(generation, *deadline); }
                        else { self->stopGeneration(generation); }
                    } catch (...) {
                        { std::lock_guard lock{ self->m_mutex }; generation->error = Error::Unknown; }
                        self->stopGeneration(generation);
                    }
                });
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
                        auto const error = failure ? Error::Unknown : result.error();
                        {
                            std::lock_guard lock{ self->m_mutex };
                            generation->error = error;
                        }
                        generation->observers.Log(server::LogEvent::AcceptFailure, error, generation->accept_error,
                            failure, nullptr, &generation->local);
                    }
                    if (failure || !result) { self->stopGeneration(generation); }
                    else { self->finishGeneration(generation); }
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
            while (!generation->stopping && !generation->admission_closed->load()) {
                auto connection = Connection::Create(self->m_runtime->Context().get_executor(),
                    { .max_write_size = self->m_options.max_response_bytes });
                if (!connection) { co_return std::unexpected{ connection.error() }; }
                auto [error] = co_await generation->acceptor->async_accept((*connection)->SocketForInitialization(),
                    asio::as_tuple(asio::use_awaitable));
                if (generation->stopping || generation->admission_closed->load()) { (*connection)->Close(); break; }
                if (error) { generation->accept_error = error; co_return std::unexpected{ Error::Connection }; }
                if (generation->sessions.size() >= self->m_options.max_connections) {
                    self->logRejected(*generation, **connection, Error::Busy);
                    (*connection)->Close();
                    continue;
                }
                if (auto configured = configure_session_socket((*connection)->SocketForInitialization(), self->m_options)) {
                    self->logRejected(*generation, **connection, Error::Connection, configured);
                    (*connection)->Close();
                    continue;
                }
                auto session = Session::Create(*connection, self->m_router, self->m_options,
                    self->m_runtime, generation->token, generation->observers, generation->admission_closed);
                if (!session) {
                    self->logRejected(*generation, **connection, session.error());
                    (*connection)->Close();
                    continue;
                }
                auto const id = generation->next_id++;
                generation->sessions.emplace(id, *session);
                try {
                    asio::co_spawn(self->m_executor, (*session)->Run(),
                        [self, generation, id, owner = *session](std::exception_ptr failure, SessionResult) mutable {
                            if (failure) {
                                owner->Stop();
                                generation->observers.Log(server::LogEvent::SessionFailure, Error::Unknown, {}, failure,
                                    nullptr, &generation->local);
                            }
                            generation->sessions.erase(id);
                            owner.reset();
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
         * @brief Observe an accepted socket rejection without exposing endpoint/observer failures to accept.
         * @param generation Immutable logger/local endpoint snapshot.
         * @param connection Initialized socket, before any connection-strand I/O.
         * @param error Rejection reason.
         * @param native_error Optional platform initialization failure.
         */
        auto logRejected(
            Generation const& generation,
            Connection& connection,
            Error error,
            asio::error_code native_error = {}
        ) const noexcept -> void {
            if (!generation.observers.logger) { return; }
            try {
                asio::error_code endpoint_error;
                auto endpoint = connection.SocketForInitialization().remote_endpoint(endpoint_error);
                std::optional<Endpoint> remote;
                if (!endpoint_error) { remote = Endpoint{ endpoint.address().to_string(), endpoint.port() }; }
                generation.observers.Log(server::LogEvent::ConnectionRejected, error, native_error, {},
                    remote ? &*remote : nullptr, &generation.local);
            } catch (...) {} // Rejection must still close the socket if diagnostics cannot be constructed.
        }

        /**
         * @brief Close the acceptor and publish the new-request fence without canceling begun requests.
         * @param generation Owned strand-confined state.
         */
        auto closeAdmission(std::shared_ptr<Generation> const& generation) -> void {
            if (generation->stopping) { return; }
            generation->stopping = true;
            generation->admission_closed->store(true);
            {
                std::lock_guard lock{ m_mutex };
                if (m_active == generation) { m_state = State::Stopping; }
            }
            asio::error_code ignored;
            generation->acceptor->close(ignored);
            m_changed.notify_all();
        }

        /**
         * @brief Arm a deadline callback, rearming only after an earlier cutoff canceled the previous wait.
         * @param generation Owned generation retained until timer cancellation or expiry has drained.
         */
        auto armShutdownTimer(std::shared_ptr<Generation> const& generation) -> void {
            generation->shutdown_timer->expires_at(*generation->shutdown_deadline);
            generation->shutdown_timer->async_wait([self = shared_from_this(), generation](asio::error_code error) {
                generation->timer_pending = false;
                if (!generation->aborting && !generation->sessions.empty()) {
                    if (!error) {
                        { std::lock_guard lock{ self->m_mutex }; generation->shutdown_error = Error::ShutdownTimeout; }
                        self->stopGeneration(generation);
                    } else if (error == asio::error::operation_aborted) {
                        self->armShutdownTimer(generation);
                    } else {
                        { std::lock_guard lock{ self->m_mutex }; generation->error = Error::Unknown; }
                        self->stopGeneration(generation);
                    }
                }
                self->finishGeneration(generation);
            });
            generation->timer_pending = true;
        }

        /**
         * @brief Retire idle sessions and let active requests finish before the earliest requested deadline.
         * @param generation Owned strand-confined state.
         * @param deadline Absolute cutoff; later requests can shorten but never extend it.
         */
        auto drainGeneration(std::shared_ptr<Generation> const& generation, Clock::time_point deadline) -> void {
            if (generation->finished || generation->aborting) { return; }
            bool const first = !generation->stopping;
            closeAdmission(generation);
            if (first) {
                for (auto const& [id, session] : generation->sessions) {
                    auto operation = session->Drain();
                    ++generation->drain_commands;
                    try {
                        asio::co_spawn(m_executor, std::move(operation),
                            [self = shared_from_this(), generation](std::exception_ptr failure) {
                                --generation->drain_commands;
                                if (failure) {
                                    { std::lock_guard lock{ self->m_mutex }; generation->error = Error::Unknown; }
                                    self->stopGeneration(generation);
                                } else { self->finishGeneration(generation); }
                            });
                    } catch (...) { --generation->drain_commands; throw; }
                }
            }
            if (!generation->sessions.empty()
                && (!generation->shutdown_deadline || deadline < *generation->shutdown_deadline)) {
                generation->shutdown_deadline = deadline;
                if (!generation->shutdown_timer) {
                    generation->shutdown_timer = std::make_shared<asio::steady_timer>(m_executor);
                }
                if (generation->timer_pending) { generation->shutdown_timer->expires_at(deadline); }
                else { armShutdownTimer(generation); }
            }
            finishGeneration(generation);
        }

        /**
         * @brief Cancel accept and every registered connection before checking the drain predicate.
         * @param generation Owned strand-confined state; repeated or late calls are harmless.
         */
        auto stopGeneration(std::shared_ptr<Generation> const& generation) -> void {
            if (generation->finished) { return; }
            closeAdmission(generation);
            if (!generation->aborting) {
                generation->aborting = true;
                if (!generation->sessions.empty()) {
                    std::lock_guard lock{ m_mutex };
                    if (!generation->shutdown_error) { generation->shutdown_error = Error::Canceled; }
                }
                if (generation->timer_pending) { generation->shutdown_timer->cancel(); }
                for (auto const& [id, session] : generation->sessions) { session->Stop(); }
            }
            finishGeneration(generation);
        }

        /**
         * @brief Thaw routes only after both accept and every session root have completed.
         * @param generation Owned server-strand state.
         */
        auto finishGeneration(std::shared_ptr<Generation> const& generation) -> void {
            if (!generation->stopping || !generation->accept_done || !generation->sessions.empty()
                || generation->drain_commands != 0 || generation->finished) { return; }
            if (generation->timer_pending) { generation->shutdown_timer->cancel(); return; }
            generation->finished = true;
            generation->runtime_stop.reset();
            generation->observers = {};
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
            for (auto const& waiter : generation->waiters) { waiter->expires_at(Clock::time_point::min()); }
            generation->waiters.clear();
            m_changed.notify_all();
        }

        /**
         * @brief Join one generation, escalating caller cancellation and retiring its callback before returning.
         * @param self Owned server, retained before initial suspension.
         * @param generation Owned generation selected before dispatch.
         * @return Whether the calling Task requested cancellation during this drain.
         */
        static auto stopAndWait(
            std::shared_ptr<ServerImpl> self,
            std::shared_ptr<Generation> generation
        ) -> asio::awaitable<bool> {
            co_await asio::this_coro::throw_if_cancelled(false);
            auto cancellation = co_await asio::this_coro::cancellation_state;
            if (generation->finished) { co_return cancellation.cancelled() != asio::cancellation_type::none; }
            auto waiter = std::make_shared<asio::steady_timer>(self->m_executor,
                Clock::time_point::max());
            generation->waiters.push_back(waiter);
            /**
             * @brief Remove the upstream cancellation callback before its owned coroutine state retires.
             */
            struct SlotScope {
                decltype(std::declval<asio::cancellation_signal&>().slot()) slot; ///< Owned operation's slot.
                /**
                 * @brief Release handler captures on every normal or exceptional exit.
                 */
                ~SlotScope() { if (slot.is_connected()) { slot.clear(); } }
            } scope{ cancellation.slot() };
            if (scope.slot.is_connected()) {
                scope.slot.assign([self, generation](asio::cancellation_type) { self->stopGeneration(generation); });
            }
            if (cancellation.cancelled() != asio::cancellation_type::none) { self->stopGeneration(generation); }
            asio::cancellation_signal neutral;
            static_cast<void>(co_await waiter->async_wait(asio::bind_cancellation_slot(neutral.slot(),
                asio::as_tuple(asio::use_awaitable))));
            co_return cancellation.cancelled() != asio::cancellation_type::none;
        }

        /**
         * @brief Preserve immediate asynchronous Stop while handling idle targets without dispatch.
         * @param self Owned server retained before initial suspension.
         */
        static auto stopTask(std::shared_ptr<ServerImpl> self) -> Task<void> {
            auto work = asio::make_work_guard(self->m_executor);
            auto generation = self->requestStop();
            if (!generation) { co_return; }
            if (co_await ToTask(self->m_executor, stopAndWait(self, generation))) { throw_error(Error::Canceled); }
        }

        /**
         * @brief Start the grace budget on the original Task scheduler and retain target work through drain.
         * @param self Owned server retained before initial suspension.
         * @param grace Nonnegative capacity budget, validated before changing lifecycle state.
         * @return Complete shutdown outcome; cancellation first forces and joins remaining sessions.
         */
        static auto gracefulStopTask(
            std::shared_ptr<ServerImpl> self,
            Clock::duration grace
        ) -> Task<Result<void>> {
            auto deadline = shutdownDeadline(grace);
            if (!deadline) { co_return std::unexpected{ deadline.error() }; }
            auto work = asio::make_work_guard(self->m_executor);
            auto generation = self->requestStop(*deadline);
            if (!generation) { co_return Result<void>{}; }
            try {
                if (co_await ToTask(self->m_executor, stopAndWait(self, generation))) {
                    co_return std::unexpected{ Error::Canceled };
                }
            } catch (std::system_error const& error) {
                if (error.code() == Error::Canceled) { co_return std::unexpected{ Error::Canceled }; }
                throw;
            }
            co_return self->shutdownResult(generation);
        }
    };

} // namespace mhttp::impl
