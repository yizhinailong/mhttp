/**
 * @file client_impl.cppm
 * @brief Serialize lazy client connections, bounded HTTP exchanges, deadlines, and cancellation.
 */
module mhttp:client.impl.client;

import std;
import asio;
import :core.impl.io_error;
import :core.impl.connection;
import :core.runtime;
import :core.parser;
import :client.request_policy;
import :client.redirect_policy;
import :client.response;
import :utils.base64;

namespace mhttp::impl {

    /**
     * @brief Own one single-flight HTTP connection on a client strand, independently of its public wrapper.
     * @note The borrowed Runtime must outlive this owner and all issued Tasks. Atomic publication is
     *       limited to Stop epochs and IsOpen; the client strand owns resolver/socket/phase state.
     */
    class ClientImpl : public std::enable_shared_from_this<ClientImpl> {
    private:
        using Clock = std::chrono::steady_clock; ///< Monotonic phase clock.
        using Duration = Clock::duration; ///< Relative I/O deadline type.

        /**
         * @brief Keep a retired connection deadline from canceling a later operation.
         */
        struct ConnectDeadline {
            asio::steady_timer timer; ///< Timer serialized with resolver and connection initialization.
            bool finished{}; ///< Completion has retired this timer.
            bool expired{}; ///< This timer won and canceled connection establishment.

            /**
             * @brief Bind a phase timer to the client's strand.
             * @param executor Client executor.
             */
            explicit ConnectDeadline(asio::any_io_executor executor) : timer{ executor } {}
        };

        /**
         * @brief Release the in-flight slot on every return or unexpected exception.
         */
        struct SendScope {
            ClientImpl& owner; ///< Coroutine-owned implementation, alive until scope destruction.
            bool succeeded{}; ///< Keep a reusable connection only after a complete successful exchange.

            /**
             * @brief Retire the slot and any incomplete connection before another Send starts.
             */
            ~SendScope() {
                owner.retireConnectDeadline();
                owner.m_busy = false;
                if (!succeeded) { owner.closeConnection(); }
            }
        };

        /**
         * @brief Retain an idle wait until cancellation/readability is joined before the next request phase.
         */
        struct IdleWatch {
            std::shared_ptr<Connection> connection; ///< Transport owner through wait completion.
            asio::cancellation_signal cancel; ///< Cancels only the readiness wait, preserving the socket.
            asio::cancellation_signal neutral; ///< Drain must complete even if the next Send is canceled.
            asio::experimental::channel<void(asio::error_code)> done; ///< One buffered completion on the client strand.
            bool stopping{}; ///< A new request deliberately retires this watch.
            bool finished{}; ///< The wait and its receive guard have drained.

            /**
             * @brief Bind readiness publication and retain the observed transport.
             * @param executor Client strand, shared by every callback touching this state.
             * @param transport Current complete-response connection.
             */
            IdleWatch(asio::any_io_executor executor, std::shared_ptr<Connection> transport) :
                connection{ std::move(transport) }, done{ executor, 1 } {}
        };

        std::shared_ptr<RuntimeImpl> m_runtime; ///< Context owner; public lifetime contract still requires Runtime.
        asio::any_io_executor m_executor; ///< Strand serializing every mutable network field below.
        asio::ip::tcp::resolver m_resolver; ///< Lazy asynchronous DNS, canceled only on m_executor.
        std::optional<Url> m_origin; ///< Immutable validated constructor origin and default target.
        std::optional<Error> m_initialization_error; ///< Immutable constructor validation failure.
        client::ClientOptions m_options; ///< Owned configuration snapshot.
        std::mutex m_headers_mutex; ///< Protects configuration accessed outside the client strand.
        Headers m_default_headers; ///< Fallback fields copied synchronously when a Task is issued.
        std::shared_ptr<Connection> m_connection; ///< Current connecting or connected transport.
        std::optional<Url> m_connection_origin; ///< Actual transport origin, distinct from the constructor URL.
        std::shared_ptr<IdleWatch> m_idle_watch; ///< Idle peer-close detection, retired before another Send.
        std::shared_ptr<ConnectDeadline> m_connect_deadline; ///< Active connection phase only.
        std::unique_ptr<std::stop_callback<std::function<void()>>> m_runtime_stop; ///< Idle and active runtime cancellation.
        std::stop_token m_runtime_token; ///< Generation owning the current resolver/socket.
        Endpoint m_remote; ///< Connected peer snapshot, accessed only on the strand.
        Endpoint m_local; ///< Connected local snapshot, accessed only on the strand.
        std::atomic<std::uint64_t> m_stop_epoch{}; ///< Stop invalidates issued requests before posting closure.
        std::atomic<bool> m_open{}; ///< Published connected state, never reads a socket from another thread.
        std::uint64_t m_active_epoch{}; ///< Stop epoch of the active request or idle connection.
        bool m_busy{}; ///< Single-flight slot, owned by the client strand.

    public:
        /**
         * @brief Retain configuration without resolving or opening a socket.
         * @param runtime Borrowed longer-lived execution owner.
         * @param origin Owned validated URL or constructor error.
         * @param options Owned buffered-client configuration.
         */
        ClientImpl(Runtime& runtime, Result<Url> origin, client::ClientOptions options) :
            m_runtime{ runtime.m_impl }, m_executor{ asio::make_strand(m_runtime->Context()) },
            m_resolver{ m_executor }, m_options{ std::move(options) } {
            m_default_headers = std::move(m_options.default_headers);
            if (origin) { m_origin.emplace(std::move(*origin)); }
            else { m_initialization_error = origin.error(); }
            auto valid = detail::validate_client_options(m_options);
            if (!m_initialization_error && !valid) { m_initialization_error = valid.error(); }
        }

        /**
         * @brief Close a retained idle transport even if the public Stop post outlives this last owner.
         * @note Active Send frames retain this owner; only independently owned idle waits can remain here.
         */
        ~ClientImpl() {
            if (m_connection) { m_connection->Close(); }
        }

        /**
         * @brief Inspect immutable construction errors without starting an operation.
         * @return Validation failure, if present.
         */
        [[nodiscard]] auto InitializationError() const noexcept -> std::optional<Error> { return m_initialization_error; }

        /**
         * @brief Borrow the normalized constructor host, independent of network state.
         * @return Bare host or empty for invalid construction.
         */
        [[nodiscard]] auto Host() const noexcept -> std::string_view { return m_origin ? m_origin->Host() : std::string_view{}; }

        /**
         * @brief Inspect the immutable constructor port.
         * @return Port or zero for invalid construction.
         */
        [[nodiscard]] auto Port() const noexcept -> std::uint16_t { return m_origin ? m_origin->Port() : 0; }

        /**
         * @brief Observe published connection state without touching the socket.
         * @return True after connection establishment until a known close/Stop/failure.
         */
        [[nodiscard]] auto IsOpen() const noexcept -> bool { return m_open.load(); }

        /**
         * @brief Issue an owned lazy Send whose state survives destruction of the public wrapper.
         * @param request Owned method/target/fields/body, retained before initial suspension.
         * @return Complete response or a recoverable validation, transport, timeout, Busy, or cancellation error.
         */
        [[nodiscard]] auto Send(client::Request request) -> Task<Result<client::Response>> {
            std::lock_guard lock{ m_headers_mutex };
            return sendTask(shared_from_this(), std::move(request), m_default_headers, m_stop_epoch.load());
        }

        /**
         * @brief Replace fallback fields for subsequently issued Tasks from any thread.
         * @param headers Owned validated fields; explicit request fields still take precedence.
         * @return Success or InvalidHeaders without changing the previous configuration.
         */
        [[nodiscard]] auto SetDefaultHeaders(Headers headers) -> Result<void> {
            auto valid = detail::validate_outgoing_host(headers, Version::Http10);
            if (!valid) { return std::unexpected{ valid.error() }; }
            std::lock_guard lock{ m_headers_mutex };
            m_default_headers = std::move(headers);
            return {};
        }

        /**
         * @brief Set a Basic Authorization fallback without retaining borrowed credential views.
         * @param username User identifier; a colon is invalid because it separates the password.
         * @param password Password, which may contain colons and is copied into the encoded value.
         * @return Success or InvalidRequest for a colon in username; failure preserves prior fields.
         */
        [[nodiscard]] auto SetBasicAuth(
            std::string_view username,
            std::string_view password
        ) -> Result<void> {
            if (username.contains(':')) { return std::unexpected{ Error::InvalidRequest }; }
            auto value = std::string{ "Basic " } + Base64Encode(std::string{ username } + ":" + std::string{ password });
            std::lock_guard lock{ m_headers_mutex };
            return m_default_headers.Set("Authorization", value);
        }

        /**
         * @brief Set a Bearer Authorization fallback for subsequently issued Tasks.
         * @param token Nonempty token using the RFC 6750 b64token alphabet and optional trailing padding.
         * @return Success or InvalidHeaders; invalid tokens preserve the preceding authorization.
         */
        [[nodiscard]] auto SetBearerToken(std::string_view token) -> Result<void> {
            bool padding{};
            if (token.empty() || token.front() == '=') { return std::unexpected{ Error::InvalidHeaders }; }
            for (unsigned char value : token) {
                if (value == '=') { padding = true; continue; }
                bool const allowed = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
                    || (value >= '0' && value <= '9') || std::string_view{ "-._~+/" }.contains(static_cast<char>(value));
                if (padding || !allowed) { return std::unexpected{ Error::InvalidHeaders }; }
            }
            std::lock_guard lock{ m_headers_mutex };
            return m_default_headers.Set("Authorization", std::string{ "Bearer " } + std::string{ token });
        }

        /**
         * @brief Cancel issued requests and close the current connection from any thread.
         * @note Future Send calls use a new epoch and may reconnect. A weak post avoids retaining an
         *       idle runtime through its own unexecuted queue when a never-started client is destroyed.
         */
        auto Stop() -> void {
            auto const epoch = m_stop_epoch.fetch_add(1) + 1;
            m_open.store(false);
            asio::post(m_executor, [weak = weak_from_this(), epoch] {
                if (auto self = weak.lock(); self && self->m_active_epoch < epoch) { self->cancelNetwork(); }
            });
        }

    private:
        /**
         * @brief Test explicit Stop or runtime generation cancellation on the client strand.
         * @param epoch Epoch captured when this request was issued.
         * @return Whether no further request I/O may begin.
         */
        [[nodiscard]] auto isCanceled(std::uint64_t epoch) const noexcept -> bool {
            return m_stop_epoch.load() != epoch || m_runtime_token.stop_requested();
        }

        /**
         * @brief Close the current transport and clear its published metadata on the strand.
         */
        auto closeConnection() -> void {
            m_open.store(false);
            if (m_idle_watch) {
                m_idle_watch->stopping = true;
                if (!m_idle_watch->finished) { m_idle_watch->cancel.emit(asio::cancellation_type::all); }
                m_idle_watch.reset();
            }
            if (m_connection) { m_connection->Close(); m_connection.reset(); }
            m_connection_origin.reset();
            m_remote = {};
            m_local = {};
        }

        /**
         * @brief Detect idle FIN/reset or unsolicited bytes without consuming a future response.
         * @note The connection executor is nested in the client strand, so all wait callbacks remain serialized.
         */
        auto watchIdleConnection() -> void {
            if (!m_connection) { return; }
            auto watch = std::make_shared<IdleWatch>(m_executor, m_connection);
            m_idle_watch = watch;
            asio::co_spawn(m_executor, watch->connection->WaitForPeerData(), asio::bind_cancellation_slot(watch->cancel.slot(),
                [weak = weak_from_this(), watch](std::exception_ptr failure, Result<void> result) {
                    watch->finished = true;
                    bool canceled = !failure && !result && result.error() == Error::Canceled;
                    if (failure) {
                        try { std::rethrow_exception(failure); }
                        catch (std::system_error const& error) { canceled = error.code() == asio::error::operation_aborted || error.code() == Error::Canceled; }
                        catch (...) {}
                    }
                    if (auto self = weak.lock(); self && self->m_idle_watch == watch
                        && self->m_connection == watch->connection && !(watch->stopping && canceled)) {
                        self->closeConnection();
                    }
                    static_cast<void>(watch->done.try_send(asio::error_code{}));
                }));
        }

        /**
         * @brief Join idle-wait cancellation before request I/O can reuse its receive direction or socket.
         * @return Completion after the wait and callback have both drained.
         */
        auto stopIdleWatch() -> asio::awaitable<void> {
            auto watch = m_idle_watch;
            if (!watch) { co_return; }
            watch->stopping = true;
            if (!watch->finished) {
                watch->cancel.emit(asio::cancellation_type::all);
                co_await watch->done.async_receive(asio::bind_cancellation_slot(watch->neutral.slot(), asio::use_awaitable));
            }
            if (m_idle_watch == watch) { m_idle_watch.reset(); }
        }

        /**
         * @brief Cancel DNS/connection establishment and close current byte transfers on the strand.
         */
        auto cancelNetwork() -> void {
            m_resolver.cancel();
            closeConnection();
        }

        /**
         * @brief Link the owning runtime generation while preserving cancellation of idle sockets.
         * @param token Snapshot of the currently running generation.
         */
        auto linkRuntime(std::stop_token token) -> void {
            if (m_runtime_token == token && m_runtime_stop) { return; }
            closeConnection();
            m_runtime_stop.reset();
            m_runtime_token = token;
            m_runtime_stop = std::make_unique<std::stop_callback<std::function<void()>>>(token,
                [weak = weak_from_this(), token] {
                    if (auto self = weak.lock()) {
                        asio::post(self->m_executor, [weak, token] {
                            if (auto owner = weak.lock(); owner && owner->m_runtime_token == token) { owner->cancelNetwork(); }
                        });
                    }
                });
        }

        /**
         * @brief Bridge client I/O back to the caller's original Task scheduler and normalize pre-start cancellation.
         * @param self Owned client retained before initial suspension.
         * @param request Owned outgoing data.
         * @param headers Issue-time fallback field snapshot.
         * @param epoch Issue-time cancellation epoch.
         * @return Owned response or a recoverable failure.
         */
        static auto sendTask(
            std::shared_ptr<ClientImpl> self,
            client::Request request,
            Headers headers,
            std::uint64_t epoch
        ) -> Task<Result<client::Response>> {
            if (self->m_initialization_error) { co_return std::unexpected{ *self->m_initialization_error }; }
            if (self->m_stop_epoch.load() != epoch) { co_return std::unexpected{ Error::Canceled }; }
            auto token = self->m_runtime->StopToken();
            if (!token) { co_return std::unexpected{ token.error() }; }
            try {
                co_return co_await ToTask(self->m_executor, send(self, std::move(request), std::move(headers), epoch, *token));
            } catch (std::system_error const& error) {
                if (error.code() == Error::Canceled) { co_return std::unexpected{ Error::Canceled }; }
                throw;
            }
        }

        /**
         * @brief Resolve and connect lazily under one deadline, retaining the transport through initialization.
         * @param epoch Current Send cancellation epoch.
         * @param origin Effective destination for this hop, retained by the awaiting Send frame.
         * @return Success or classified DNS/connect/deadline/cancellation failure.
         */
        auto connect(std::uint64_t epoch, Url const& origin) -> asio::awaitable<Result<void>> {
            auto deadline = std::make_shared<ConnectDeadline>(m_executor);
            m_connect_deadline = deadline;
            deadline->timer.expires_after(m_options.connect_timeout);
            deadline->timer.async_wait([weak = weak_from_this(), deadline](asio::error_code error) {
                if (!error && !deadline->finished) {
                    deadline->expired = true;
                    if (auto self = weak.lock()) { self->cancelNetwork(); }
                }
            });
            auto const [resolve_error, endpoints] = co_await m_resolver.async_resolve(
                std::string{ origin.Host() }, std::to_string(origin.Port()), asio::as_tuple(asio::use_awaitable));
            if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
            if (deadline->expired) { co_return std::unexpected{ Error::ConnectionTimeout }; }
            if (auto failure = map_io_error(resolve_error, IoOperation::Resolve)) { co_return std::unexpected{ *failure }; }
            auto created = Connection::Create(m_executor, { .max_write_size = m_options.max_request_bytes });
            if (!created) { co_return std::unexpected{ created.error() }; }
            m_connection = *created;
            auto connection = m_connection;
            auto const [connect_error, endpoint] = co_await asio::async_connect(
                connection->SocketForInitialization(), endpoints, asio::as_tuple(asio::use_awaitable));
            if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
            if (deadline->expired) { co_return std::unexpected{ Error::ConnectionTimeout }; }
            if (auto failure = map_io_error(connect_error, IoOperation::Connect)) { co_return std::unexpected{ *failure }; }
            auto& socket = connection->SocketForInitialization();
            asio::error_code error;
            socket.set_option(asio::ip::tcp::no_delay{ true }, error);
            if (error) { co_return std::unexpected{ Error::Connection }; }
            auto local = socket.local_endpoint(error);
            if (error) { co_return std::unexpected{ Error::Connection }; }
            m_remote = { endpoint.address().to_string(), endpoint.port() };
            m_local = { local.address().to_string(), local.port() };
            m_connection_origin = origin;
            m_open.store(true);
            if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
            co_return Result<void>{};
        }

        /**
         * @brief Retire a phase deadline on every normal/error exit before any new network phase starts.
         */
        auto retireConnectDeadline() -> void {
            if (!m_connect_deadline) { return; }
            m_connect_deadline->finished = true;
            static_cast<void>(m_connect_deadline->timer.cancel());
            m_connect_deadline.reset();
        }

        /**
         * @brief Reject advertised built-in compression codecs while leaving unknown content codings untouched.
         * @param headers Validated final response fields.
         * @return Whether a decoder not implemented by the buffered MVP is required.
         */
        [[nodiscard]] static auto unsupportedEncoding(Headers const& headers) -> bool {
            for (auto const& [name, value] : headers) {
                if (!utils::iequals(name, "Content-Encoding")) { continue; }
                for (auto token : utils::split(value, ',')) {
                    token = utils::trim_ows(token);
                    if (utils::iequals(token, "gzip") || utils::iequals(token, "deflate")
                        || utils::iequals(token, "br") || utils::iequals(token, "zstd")) { return true; }
                }
            }
            return false;
        }

        /**
         * @brief Read one bounded final response with one deadline through headers, interim messages, and body.
         * @param request Prepared outgoing metadata used for HEAD and persistence decisions.
         * @param epoch Current request cancellation epoch.
         * @return Immutable complete response or classified read/framing/content-encoding failure.
         */
        auto readResponse(
            detail::PreparedClientRequest const& request,
            std::uint64_t epoch
        ) -> asio::awaitable<Result<client::Response>> {
            detail::ResponseParser parser{ request.request.Method().value_or(Method::Get), detail::client_parser_limits(m_options) };
            auto const until = Clock::now() + m_options.read_timeout;
            auto connection = m_connection;
            std::string input, body;
            std::size_t offset{};
            bool eof{};
            for (;;) {
                if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                auto step = parser.Feed(std::span{ input.data(), input.size() }.subspan(offset));
                offset += step.consumed;
                if (step.event == detail::ParseEvent::NeedMore && eof) { step = parser.Finish(); }
                switch (step.event) {
                case detail::ParseEvent::HeadersComplete:
                    if (unsupportedEncoding(parser.Head().headers)) { co_return std::unexpected{ Error::UnsupportedContentEncoding }; }
                    break;
                case detail::ParseEvent::Body:
                    body.append(step.data.data(), step.data.size());
                    break;
                case detail::ParseEvent::Failed:
                    co_return std::unexpected{ step.failure->error };
                case detail::ParseEvent::Complete: {
                    if (offset != input.size()) { co_return std::unexpected{ Error::ProtocolError }; }
                    bool const keep_alive = request.head.keep_alive && parser.Framing().keep_alive && !eof;
                    co_return detail::ClientResponseAccess::Create(parser.Head(), std::move(body), parser.Trailers(),
                        keep_alive, m_remote, m_local);
                }
                case detail::ParseEvent::NeedMore: {
                    auto const remaining = until - Clock::now();
                    if (remaining <= Duration::zero()) { co_return std::unexpected{ Error::ReadTimeout }; }
                    auto read = co_await connection->ReadSome(remaining);
                    if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                    if (!read) {
                        if (read.error() != Error::ConnectionClosed) { co_return std::unexpected{ read.error() }; }
                        eof = true;
                        input.clear();
                    } else { input = std::move(*read); }
                    offset = 0;
                    break;
                }
                }
            }
        }

        /**
         * @brief Run a bounded redirect chain on the client strand without admitting overlapping Sends.
         * @param self Owned implementation for the entire operation.
         * @param request Owned outgoing data.
         * @param headers Issue-time fallback fields, isolated from later configuration changes.
         * @param epoch Issue-time Stop epoch.
         * @param token Client runtime generation cancellation.
         * @return Complete response or recoverable validation/I/O/lifecycle failure.
         */
        static auto send(
            std::shared_ptr<ClientImpl> self,
            client::Request request,
            Headers headers,
            std::uint64_t epoch,
            std::stop_token token
        ) -> asio::awaitable<Result<client::Response>> {
            co_await asio::this_coro::throw_if_cancelled(false);
            if (self->m_busy) { co_return std::unexpected{ Error::Busy }; }
            if (self->m_stop_epoch.load() != epoch || token.stop_requested()) { co_return std::unexpected{ Error::Canceled }; }
            auto options = self->m_options;
            options.default_headers = std::move(headers);
            auto prepared = detail::prepare_client_request(std::move(request), *self->m_origin, options);
            if (!prepared) { co_return std::unexpected{ prepared.error() }; }
            auto current = Url::Resolve(*self->m_origin, prepared->target == "*" ? "/" : prepared->target);
            if (!current) { co_return std::unexpected{ current.error() }; }
            self->m_busy = true;
            SendScope scope{ *self };
            if (self->m_active_epoch != epoch) { self->closeConnection(); }
            self->m_active_epoch = epoch;
            self->linkRuntime(token);
            co_await self->stopIdleWatch();
            std::size_t followed{};
            for (;;) {
                if (self->isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (self->m_connection_origin && !detail::same_client_origin(*self->m_connection_origin, *current)) {
                    self->closeConnection();
                }
                bool const reused = static_cast<bool>(self->m_connection);
                if (!self->m_connection) {
                    auto connected = co_await self->connect(epoch, *current);
                    self->retireConnectDeadline();
                    if (!connected) { co_return std::unexpected{ connected.error() }; }
                }
                auto const until = Clock::now() + self->m_options.write_timeout;
                auto connection = self->m_connection;
                auto written = co_await connection->WriteAllWithProgress(prepared->head.bytes, until - Clock::now());
                if (self->isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (reused && written.error == Error::ConnectionClosed && written.bytes_transferred == 0) {
                    self->closeConnection();
                    auto connected = co_await self->connect(epoch, *current);
                    self->retireConnectDeadline();
                    if (!connected) { co_return std::unexpected{ connected.error() }; }
                    connection = self->m_connection;
                    written = co_await connection->WriteAllWithProgress(prepared->head.bytes, until - Clock::now());
                }
                if (self->isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (written.error) { co_return std::unexpected{ *written.error }; }
                auto wire_body = options.max_redirects == 0 ? std::move(prepared->request.m_impl->body)
                    : std::string{ prepared->request.Body() };
                auto body = co_await connection->WriteAll(std::move(wire_body), until - Clock::now());
                if (self->isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (!body) { co_return std::unexpected{ body.error() }; }
                auto response = co_await self->readResponse(*prepared, epoch);
                if (!response) { co_return response; }
                if (self->isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (!response->KeepAlive()) { self->closeConnection(); }
                auto destination = detail::client_redirect_target(*current, response->Status(), response->Headers(),
                    options.max_redirects, followed);
                if (!destination) { co_return std::unexpected{ destination.error() }; }
                if (*destination) {
                    auto next = std::move(**destination);
                    detail::redirect_client_request(prepared->request, options.default_headers, *current, next, response->Status());
                    current = std::move(next);
                    prepared = detail::prepare_client_request(std::move(prepared->request), *current, options);
                    if (!prepared) { co_return std::unexpected{ prepared.error() }; }
                    ++followed;
                    continue;
                }
                self->watchIdleConnection();
                scope.succeeded = true;
                co_return response;
            }
        }
    };

} // namespace mhttp::impl
