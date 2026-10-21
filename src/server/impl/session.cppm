/**
 * @file session.cppm
 * @brief Run a bounded HTTP/1.x request/response loop on one connection's serialized executor.
 */
module mhttp:server.impl.session;

import std;
import asio;
import :core.impl.io_error;
import :core.impl.connection;
import :core.impl.bridge;
import :core.parser;
import :server.router;
import :server.response_policy;
import :server.impl.observers;
import :server.impl.request_input;

namespace mhttp::impl {

    /**
     * @brief Report terminal session state after every request, callback, and transport operation has drained.
     */
    struct SessionResult {
        std::size_t responses{}; ///< Successfully written final responses, excluding interim responses.
        std::optional<Error> error; ///< Terminal protocol/transport/configuration failure, absent on normal closure.
        std::exception_ptr failure; ///< Contained application or unexpected internal exception for logging.
    };

    /**
     * @brief Own a one-shot connection loop, immutable router ownership, and request-scoped buffers.
     * @note The server freezes routing before creating sessions. All mutable session state runs on the
     *       connection strand. Run retains ownership before initial suspension; Stop is thread-safe and
     *       cancellation drains nested Tasks, including blocking work already running through Offload.
     */
    class Session : public std::enable_shared_from_this<Session> {
    private:
        using Clock = std::chrono::steady_clock; ///< Clock shared by whole-phase transfer deadlines.
        std::shared_ptr<Connection> m_connection; ///< Socket, transfer bounds, and serialized executor.
        std::shared_ptr<RouterImpl> m_router; ///< Configuration retained independently of the public wrapper.
        server::ServerOptions m_options; ///< Owned validated configuration snapshot.
        ServerObservers m_observers; ///< Owned immutable callback identities for this session.
        std::shared_ptr<RuntimeImpl> m_runtime; ///< Runtime supplying Offload workers, retained through all roots.
        std::stop_token m_runtime_stop; ///< Cancellation generation supplied by RuntimeImpl::Submit.
        std::stop_source m_stop_source; ///< Session cancellation for awaited user Tasks.
        Endpoint m_remote; ///< Owned initialized peer address and port.
        Endpoint m_local; ///< Owned initialized local address and port.
        std::shared_ptr<RequestInput> m_input; ///< Shared retained input for sequential request parsers.
        Clock::time_point m_request_started{}; ///< First nonempty feed time, reset before each request head.
        bool m_started{}; ///< Run is one-shot; accessed only on the connection strand.
        bool m_stopping{}; ///< Stop prevents further requests and response persistence.

        /**
         * @brief Own validated dependencies before publishing the shared session.
         * @param connection Initialized socket with sufficient write capacity for the configured response bound.
         * @param router Owned frozen configuration.
         * @param options Validated owned limits and default fields.
         * @param runtime Optional owned runtime for Offload.
         * @param runtime_stop Runtime generation whose stop cancels this session.
         * @param remote Owned peer metadata.
         * @param local Owned local metadata.
         * @param observers Owned immutable logger snapshot.
         */
        Session(
            std::shared_ptr<Connection> connection,
            std::shared_ptr<RouterImpl> router,
            server::ServerOptions options,
            std::shared_ptr<RuntimeImpl> runtime,
            std::stop_token runtime_stop,
            Endpoint remote,
            Endpoint local,
            ServerObservers observers
        ) : m_connection{ std::move(connection) }, m_router{ std::move(router) },
            m_options{ std::move(options) }, m_observers{ std::move(observers) },
            m_runtime{ std::move(runtime) }, m_runtime_stop{ runtime_stop },
            m_remote{ std::move(remote) }, m_local{ std::move(local) },
            m_input{ std::make_shared<RequestInput>(m_connection) } {}

    public:
        /**
         * @brief Snapshot configuration and initialized endpoints before starting any socket operation.
         * @param connection Initialized connection, not concurrently accessed during this call.
         * @param router Borrowed only during creation; the session retains its implementation.
         * @param options Limits; the connection write bound must be at least max_response_bytes.
         * @param runtime Optional runtime retained for blocking-work dispatch.
         * @param runtime_stop Runtime generation cancellation, captured before session submission.
         * @param observers Owned optional logging callbacks.
         * @return Owned session, InvalidRequest for invalid options, or Connection for endpoint failure.
         */
        static auto Create(
            std::shared_ptr<Connection> connection,
            server::Router const& router,
            server::ServerOptions options = {},
            std::shared_ptr<RuntimeImpl> runtime = {},
            std::stop_token runtime_stop = {},
            ServerObservers observers = {}
        ) -> Result<std::shared_ptr<Session>> {
            return Create(std::move(connection), router.m_impl, std::move(options), std::move(runtime),
                runtime_stop, std::move(observers));
        }

        /**
         * @brief Create a session from server-owned frozen routing without borrowing a public wrapper.
         * @param connection Initialized connection, not concurrently accessed during this call.
         * @param router Owned configuration frozen through the session lifetime.
         * @param options Owned validated configuration snapshot.
         * @param runtime Optional owned runtime for Offload.
         * @param runtime_stop Runtime generation cancellation.
         * @param observers Owned immutable logging snapshot.
         * @return Owned session or a configuration/endpoint failure.
         */
        static auto Create(
            std::shared_ptr<Connection> connection,
            std::shared_ptr<RouterImpl> router,
            server::ServerOptions options,
            std::shared_ptr<RuntimeImpl> runtime,
            std::stop_token runtime_stop,
            ServerObservers observers = {}
        ) -> Result<std::shared_ptr<Session>> {
            if (!connection || !router || !detail::validate_server_options(options)
                || connection->MaxWriteSize() < options.max_response_bytes) {
                return std::unexpected{ Error::InvalidRequest };
            }
            auto& socket = connection->SocketForInitialization();
            asio::error_code error;
            auto remote = socket.remote_endpoint(error);
            if (error) { return std::unexpected{ Error::Connection }; }
            auto local = socket.local_endpoint(error);
            if (error) { return std::unexpected{ Error::Connection }; }
            return std::shared_ptr<Session>{ new Session{ std::move(connection), std::move(router),
                std::move(options), std::move(runtime), runtime_stop,
                { remote.address().to_string(), remote.port() }, { local.address().to_string(), local.port() },
                std::move(observers) } };
        }

        /**
         * @brief Run once on the connection strand, retaining ownership before initial suspension.
         * @return Final state after closing; a duplicate run returns Busy without touching the active loop.
         */
        auto Run() -> asio::awaitable<SessionResult> {
            return asio::co_spawn(m_connection->Executor(), run(shared_from_this()), asio::use_awaitable);
        }

        /**
         * @brief Request cancellation from any thread and close pending socket I/O on the connection strand.
         */
        auto Stop() -> void {
            asio::dispatch(m_connection->Executor(), [self = shared_from_this()] {
                self->m_stopping = true;
                self->m_stop_source.request_stop();
                self->m_connection->Close();
            });
        }

    private:
        /**
         * @brief Own the root frame and close the socket on every completion and exception path.
         * @param self Owned session state, retained before initial suspension.
         * @return Session completion including any exception, after nested operations drain.
         */
        static auto run(std::shared_ptr<Session> self) -> asio::awaitable<SessionResult> {
            if (self->m_started) { co_return SessionResult{ .error = Error::Busy }; }
            self->m_started = true;
            SessionResult result;
            try {
                std::stop_callback stopped{ self->m_runtime_stop, [weak = std::weak_ptr{ self }] {
                    if (auto session = weak.lock()) { session->Stop(); }
                } };
                co_await self->loop(result);
            } catch (std::system_error const& error) {
                result.error = error.code() == Error::Canceled ? Error::Canceled : Error::Unknown;
                result.failure = std::current_exception();
            } catch (...) {
                result.error = Error::Unknown;
                result.failure = std::current_exception();
            }
            self->m_connection->Close();
            bool const expected_close = result.error == Error::Canceled
                || (result.error == Error::ConnectionClosed && self->m_request_started == Clock::time_point{});
            if (!expected_close && (result.error || result.failure)) {
                self->m_observers.Log(server::LogEvent::SessionFailure, result.error.value_or(Error::Unknown), {},
                    result.failure, &self->m_remote, &self->m_local);
            }
            co_return result;
        }

        /**
         * @brief Feed retained bytes, reading only when exhausted, without resetting the phase deadline.
         * @param parser Borrowed parser whose input views are consumed before the next call.
         * @param deadline Absolute deadline for the complete header or body phase.
         * @param saw_input Records any request bytes, distinguishing idle EOF from a truncated request.
         * @return Next parser event or a contextual read failure.
         */
        auto readEvent(
            detail::RequestParser& parser,
            Clock::time_point deadline,
            bool& saw_input
        ) -> asio::awaitable<Result<detail::ParseStep>> {
            auto event = co_await m_input->ReadEvent(parser, deadline);
            m_request_started = m_input->Started();
            saw_input = m_request_started != Clock::time_point{};
            co_return event;
        }

        /**
         * @brief Finalize callbacks and write one bounded owned response while retaining the prepared route lease.
         * @param request Borrowed current request.
         * @param response Borrowed current response, reset to a safe 500 if serialization is rejected.
         * @param route Prepared state borrowed through response completion.
         * @param body_complete Whether the complete wire body was consumed.
         * @param force_close Protocol failure or early rejection prohibits persistence.
         * @param result Root-owned completion state and final-response counter.
         * @return Whether another request may be processed.
         */
        auto writeFinal(
            server::Request& request,
            server::Response& response,
            detail::PreparedRoute const& route,
            bool body_complete,
            bool force_close,
            SessionResult& result
        ) -> asio::awaitable<bool> {
            auto outcome = co_await AwaitTask(RouterImpl::Process(route, request, response),
                m_connection->Executor(), m_stop_source.get_token(), m_runtime);
            if (auto complete = detail::ServerRequestAccess::BodyComplete(request)) {
                body_complete = *complete;
                force_close = force_close || !body_complete;
                if (auto failure = detail::ServerRequestAccess::BodyFailure(request)) { result.error = failure->error; }
            }
            result.failure = outcome.failure;
            if (!outcome.send_response) { result.error = Error::Unknown; co_return false; }
            outcome.close = outcome.close || force_close;
            detail::ResponseSessionState state{ result.responses + 1, body_complete, m_stopping };
            auto wire = detail::serialize_server_response(request, response, m_options, outcome, state);
            if (!wire) {
                result.error = wire.error();
                response = server::Response{};
                static_cast<void>(response.SetError(Status::InternalServerError));
                outcome.close = true;
                wire = detail::serialize_server_response(request, response, m_options, outcome, state);
                if (!wire) { co_return false; }
            }
            bool const keep_alive = wire->keep_alive;
            auto const deadline = Clock::now() + m_options.write_timeout;
            auto written = co_await m_connection->WriteAll(std::move(wire->bytes), deadline - Clock::now());
            if (!written) { result.error = written.error(); co_return false; }
            if (wire->streaming && !co_await writeStream(response, *wire, deadline, result)) { co_return false; }
            ++result.responses;
            m_observers.Access(request, response, Clock::now() - m_request_started);
            if (!keep_alive) { static_cast<void>(co_await m_connection->Shutdown()); }
            co_return keep_alive;
        }

        /**
         * @brief Await the stable owned provider and terminal framing under one cancelable response deadline.
         * @param response Borrowed finalized response; its callback moves into this frame before invocation.
         * @param wire Owned head result, including final fields and selected stream mode.
         * @param deadline Absolute deadline also used for head transmission.
         * @param result Root-owned error/exception metadata; no response count is added on failure.
         * @return Whether the complete provider and terminal write succeeded before retirement.
         */
        auto writeStream(
            server::Response& response,
            detail::SerializedServerResponse& wire,
            Clock::time_point deadline,
            SessionResult& result
        ) -> asio::awaitable<bool> {
            auto state = StreamWriterImpl::Create(m_connection, wire.body_kind,
                std::move(wire.stream_headers), m_options.stream_chunk_bytes, deadline, wire.content_length);
            StreamRetirement retirement{ state };
            std::stop_callback stopped{ m_stop_source.get_token(), [weak = std::weak_ptr{ state }] {
                if (auto owner = weak.lock()) { owner->Cancel(); }
            } };
            auto writer = detail::StreamWriterAccess::Create(state);
            auto provider = detail::ServerResponseAccess::TakeProvider(response);
            try {
                if (!provider) { state->Fail(Error::InvalidRequest); }
                else {
                    auto provided = co_await AwaitTask(provider(writer), m_connection->Executor(),
                        state->StopToken(), m_runtime);
                    if (!provided) { state->Fail(provided.error()); }
                    if (!state->ErrorValue() && !state->Finished()) {
                        auto finished = co_await state->Finish();
                        if (!finished) { state->Fail(finished.error()); }
                    }
                }
            } catch (std::system_error const& error) {
                result.failure = std::current_exception();
                state->Fail(error.code() == Error::Canceled ? Error::Canceled : Error::Unknown);
            } catch (...) {
                result.failure = std::current_exception();
                state->Fail(Error::Unknown);
            }
            if (auto error = state->ErrorValue()) { result.error = *error; co_return false; }
            co_return true;
        }

        /**
         * @brief Send a parser error with sanitized metadata, bypassing request interception and route matching.
         * @param parser Parser whose start line may be only partially validated.
         * @param status Fatal protocol error status.
         * @param result Root-owned completion state.
         */
        auto rejectHead(
            detail::RequestParser const& parser,
            Status status,
            SessionResult& result
        ) -> asio::awaitable<void> {
            detail::RequestHead head{ .version = parser.Head().version };
            if (parser.Head().method == "HEAD") { head.method = "HEAD"; }
            auto request = detail::ServerRequestAccess::Create(
                std::move(head), m_remote, m_local, m_options.trusted_proxies);
            server::Response response;
            static_cast<void>(response.SetError(status));
            auto route = m_router->PrepareError();
            static_cast<void>(co_await writeFinal(request, response, route, false, true, result));
        }

        /**
         * @brief Process headers, route preparation, complete body intake, and ordered pipelined responses.
         * @param result Root-owned final-response counter and terminal diagnostics.
         */
        auto loop(SessionResult& result) -> asio::awaitable<void> {
            while (!m_stopping) {
                detail::RequestParser parser{ detail::server_parser_limits(m_options) };
                m_input->BeginRequest();
                m_request_started = {};
                bool saw_input{};
                auto const timeout = result.responses == 0 ? m_options.read_timeout : m_options.keep_alive_timeout;
                auto event = co_await readEvent(parser, Clock::now() + timeout, saw_input);
                if (!event) {
                    result.error = event.error();
                    if (event.error() == Error::ConnectionClosed && saw_input) {
                        co_await rejectHead(parser, parser.Finish().failure->status, result);
                    } else if (event.error() == Error::ReadTimeout && saw_input) {
                        co_await rejectHead(parser, Status::RequestTimeout, result);
                    }
                    co_return;
                }
                if (event->event == detail::ParseEvent::Failed) {
                    result.error = event->failure->error;
                    co_await rejectHead(parser, event->failure->status, result);
                    co_return;
                }
                auto request = detail::ServerRequestAccess::Create(
                    parser.Head(), m_remote, m_local, m_options.trusted_proxies);
                server::Response response;
                auto route = co_await AwaitTask(m_router->Prepare(request, response),
                    m_connection->Executor(), m_stop_source.get_token(), m_runtime);
                bool body_complete = parser.Framing().kind == detail::BodyKind::None
                    || (parser.Framing().kind == detail::BodyKind::ContentLength && parser.Framing().content_length == 0);
                bool const safe_method = request.Method() == Method::Get || request.Method() == Method::Head
                    || request.Method() == Method::Options || request.Method() == Method::Trace;
                bool const unmatched_unsafe = !route.handled && !route.handler && !safe_method;
                bool const rejected_expectation = parser.Framing().expect_continue && (route.handled || !route.handler);
                bool const skip_body = route.handled || unmatched_unsafe || rejected_expectation;
                if (parser.Framing().expect_continue && !skip_body && !body_complete && !route.lazy) {
                    auto interim = co_await m_connection->WriteAll("HTTP/1.1 100 Continue\r\n\r\n", m_options.write_timeout);
                    if (!interim) { result.error = interim.error(); co_return; }
                }
                RequestBodyRetirement retirement;
                if (!skip_body && route.lazy) {
                    retirement.state = std::make_shared<RequestBodyImpl>(m_connection, m_input, std::move(parser),
                        Clock::now() + m_options.read_timeout, m_options.request_chunk_bytes,
                        m_options.write_timeout, body_complete);
                    detail::ServerRequestAccess::AttachBodyReader(request, retirement.state);
                } else if (!skip_body) {
                    std::string body;
                    auto const deadline = Clock::now() + m_options.read_timeout;
                    for (;;) {
                        event = co_await readEvent(parser, deadline, saw_input);
                        if (!event) {
                            result.error = event.error();
                            if (event.error() == Error::ReadTimeout) {
                                route.handled = true;
                                route.failure = {};
                                static_cast<void>(response.SetError(Status::RequestTimeout));
                                break;
                            }
                            if (event.error() != Error::ConnectionClosed) { co_return; }
                            event = parser.Finish();
                        }
                        if (event->event == detail::ParseEvent::Body) {
                            body.append(event->data.data(), event->data.size());
                        } else if (event->event == detail::ParseEvent::Complete) {
                            body_complete = true;
                            detail::ServerRequestAccess::SetBody(request, std::move(body));
                            detail::ServerRequestAccess::SetTrailers(request, parser.Trailers());
                            break;
                        } else if (event->event == detail::ParseEvent::Failed) {
                            result.error = event->failure->error;
                            route.handled = true;
                            route.failure = {};
                            static_cast<void>(response.SetError(event->failure->status));
                            break;
                        }
                    }
                }
                if (!co_await writeFinal(request, response, route, body_complete,
                    unmatched_unsafe || (!body_complete && !route.lazy) || rejected_expectation, result)) { co_return; }
            }
        }
    };

} // namespace mhttp::impl
