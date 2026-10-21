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
        std::shared_ptr<RuntimeImpl> m_runtime; ///< Runtime supplying Offload workers, retained through all roots.
        std::stop_token m_runtime_stop; ///< Cancellation generation supplied by RuntimeImpl::Submit.
        std::stop_source m_stop_source; ///< Session cancellation for awaited user Tasks.
        Endpoint m_remote; ///< Owned initialized peer address and port.
        Endpoint m_local; ///< Owned initialized local address and port.
        std::string m_input; ///< One owned read chunk, retaining any pipelined successor bytes.
        std::size_t m_offset{}; ///< Consumed prefix of m_input.
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
         */
        Session(
            std::shared_ptr<Connection> connection,
            std::shared_ptr<RouterImpl> router,
            server::ServerOptions options,
            std::shared_ptr<RuntimeImpl> runtime,
            std::stop_token runtime_stop,
            Endpoint remote,
            Endpoint local
        ) : m_connection{ std::move(connection) }, m_router{ std::move(router) },
            m_options{ std::move(options) }, m_runtime{ std::move(runtime) }, m_runtime_stop{ runtime_stop },
            m_remote{ std::move(remote) }, m_local{ std::move(local) } {}

    public:
        /**
         * @brief Snapshot configuration and initialized endpoints before starting any socket operation.
         * @param connection Initialized connection, not concurrently accessed during this call.
         * @param router Borrowed only during creation; the session retains its implementation.
         * @param options Limits; the connection write bound must be at least max_response_bytes.
         * @param runtime Optional runtime retained for blocking-work dispatch.
         * @param runtime_stop Runtime generation cancellation, captured before session submission.
         * @return Owned session, InvalidRequest for invalid options, or Connection for endpoint failure.
         */
        static auto Create(
            std::shared_ptr<Connection> connection,
            server::Router const& router,
            server::ServerOptions options = {},
            std::shared_ptr<RuntimeImpl> runtime = {},
            std::stop_token runtime_stop = {}
        ) -> Result<std::shared_ptr<Session>> {
            return Create(std::move(connection), router.m_impl, std::move(options), std::move(runtime), runtime_stop);
        }

        /**
         * @brief Create a session from server-owned frozen routing without borrowing a public wrapper.
         * @param connection Initialized connection, not concurrently accessed during this call.
         * @param router Owned configuration frozen through the session lifetime.
         * @param options Owned validated configuration snapshot.
         * @param runtime Optional owned runtime for Offload.
         * @param runtime_stop Runtime generation cancellation.
         * @return Owned session or a configuration/endpoint failure.
         */
        static auto Create(
            std::shared_ptr<Connection> connection,
            std::shared_ptr<RouterImpl> router,
            server::ServerOptions options,
            std::shared_ptr<RuntimeImpl> runtime,
            std::stop_token runtime_stop
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
                { remote.address().to_string(), remote.port() }, { local.address().to_string(), local.port() } } };
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
            for (;;) {
                auto input = std::span<char const>{ m_input.data(), m_input.size() }.subspan(m_offset);
                saw_input = saw_input || !input.empty();
                auto step = parser.Feed(input);
                m_offset += step.consumed;
                if (step.event != detail::ParseEvent::NeedMore) { co_return step; }
                auto bytes = co_await m_connection->ReadSome(deadline - Clock::now());
                if (!bytes) { co_return std::unexpected{ bytes.error() }; }
                m_input = std::move(*bytes);
                m_offset = 0;
            }
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
            auto written = co_await m_connection->WriteAll(std::move(wire->bytes), m_options.write_timeout);
            if (!written) { result.error = written.error(); co_return false; }
            ++result.responses;
            if (!keep_alive) { static_cast<void>(co_await m_connection->Shutdown()); }
            co_return keep_alive;
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
            auto request = detail::ServerRequestAccess::Create(std::move(head), m_remote, m_local);
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
            detail::RequestParser parser{ detail::server_parser_limits(m_options) };
            while (!m_stopping) {
                bool saw_input{};
                auto const timeout = result.responses == 0 ? m_options.read_timeout : m_options.keep_alive_timeout;
                auto event = co_await readEvent(parser, Clock::now() + timeout, saw_input);
                if (!event) {
                    result.error = event.error();
                    if (event.error() == Error::ConnectionClosed && saw_input) {
                        co_await rejectHead(parser, parser.Finish().failure->status, result);
                    }
                    co_return;
                }
                if (event->event == detail::ParseEvent::Failed) {
                    result.error = event->failure->error;
                    co_await rejectHead(parser, event->failure->status, result);
                    co_return;
                }
                auto request = detail::ServerRequestAccess::Create(parser.Head(), m_remote, m_local);
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
                if (parser.Framing().expect_continue && !skip_body && !body_complete) {
                    auto interim = co_await m_connection->WriteAll("HTTP/1.1 100 Continue\r\n\r\n", m_options.write_timeout);
                    if (!interim) { result.error = interim.error(); co_return; }
                }
                if (!skip_body) {
                    std::string body;
                    auto const deadline = Clock::now() + m_options.read_timeout;
                    for (;;) {
                        event = co_await readEvent(parser, deadline, saw_input);
                        if (!event) {
                            result.error = event.error();
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
                    unmatched_unsafe || !body_complete || rejected_expectation, result)) { co_return; }
                parser.Reset();
            }
        }
    };

} // namespace mhttp::impl
