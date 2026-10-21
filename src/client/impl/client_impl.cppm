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
import :client.impl.deadlines;
import :client.impl.callback;
import :client.transfer;
import :client.response;
import :utils.base64;
import :utils.authorization;

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
        std::shared_ptr<ClientCallback> m_callback; ///< Active awaited callback, canceled with the current network generation.
        std::shared_ptr<ConnectDeadline> m_connect_deadline; ///< Active connection phase only.
        std::unique_ptr<std::stop_callback<std::function<void()>>> m_runtime_stop; ///< Idle and active runtime cancellation.
        std::stop_token m_runtime_token; ///< Generation owning the current resolver/socket.
        Endpoint m_remote; ///< Connected peer snapshot, accessed only on the strand.
        Endpoint m_local; ///< Connected local snapshot, accessed only on the strand.
        std::atomic<std::uint64_t> m_stop_epoch{}; ///< Stop invalidates issued requests before posting closure.
        std::atomic<bool> m_open{}; ///< Published connected state, never reads a socket from another thread.
        std::atomic<bool> m_terminal{}; ///< A stopped pool permanently retires its borrowed clients.
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
         * @brief Issue a nonconsuming asynchronous health probe without connecting a lazy client.
         * @param lifetime Optional pool loan retained through probe completion.
         * @return False for no connection or observed peer closure; Busy for an overlapping transfer.
         */
        auto IsAlive(std::shared_ptr<void> lifetime = {}) -> Task<Result<bool>> {
            return aliveTask(shared_from_this(), m_stop_epoch.load(), std::move(lifetime));
        }

        /**
         * @brief Issue an owned lazy Send whose state survives destruction of the public wrapper.
         * @param request Owned method/target/fields/body, retained before initial suspension.
         * @param callbacks Owned optional asynchronous receivers and progress.
         * @param lifetime Optional pool loan retained from issuance until the complete Task frame retires.
         * @return Complete response or a recoverable validation, transport, timeout, Busy, or cancellation error.
         */
        [[nodiscard]] auto Send(
            client::Request request,
            client::TransferCallbacks callbacks = {},
            std::shared_ptr<void> lifetime = {}
        ) -> Task<Result<client::Response>> {
            std::lock_guard lock{ m_headers_mutex };
            return sendTask(shared_from_this(), std::move(request), std::move(callbacks), m_default_headers,
                m_stop_epoch.load(), std::move(lifetime));
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
            if (!detail::is_bearer_token(token)) { return std::unexpected{ Error::InvalidHeaders }; }
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

        /**
         * @brief Permanently reject future operations after the owning pool stops.
         */
        auto StopPermanently() -> void { m_terminal.store(true); Stop(); }

    private:
        /**
         * @brief Test explicit Stop or runtime generation cancellation on the client strand.
         * @param epoch Epoch captured when this request was issued.
         * @return Whether no further request I/O may begin.
         */
        [[nodiscard]] auto isCanceled(std::uint64_t epoch) const noexcept -> bool {
            return m_terminal.load() || m_stop_epoch.load() != epoch || m_runtime_token.stop_requested();
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
            if (m_callback) { m_callback->Cancel(); }
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
         * @param callbacks Owned asynchronous receivers/progress, retained through all redirect hops.
         * @param headers Issue-time fallback field snapshot.
         * @param epoch Issue-time cancellation epoch.
         * @param lifetime Owned optional pool slot, including while this lazy Task has not started.
         * @return Owned response or a recoverable failure.
         */
        static auto sendTask(
            std::shared_ptr<ClientImpl> self,
            client::Request request,
            client::TransferCallbacks callbacks,
            Headers headers,
            std::uint64_t epoch,
            std::shared_ptr<void> lifetime
        ) -> Task<Result<client::Response>> {
            if (self->m_initialization_error) { co_return std::unexpected{ *self->m_initialization_error }; }
            if (self->m_terminal.load() || self->m_stop_epoch.load() != epoch) { co_return std::unexpected{ Error::Canceled }; }
            auto token = self->m_runtime->StopToken();
            if (!token) { co_return std::unexpected{ token.error() }; }
            try {
                co_return co_await ToTask(self->m_executor, send(self, std::move(request), std::move(callbacks), std::move(headers), epoch, *token));
            } catch (std::system_error const& error) {
                if (error.code() == Error::Canceled) { co_return std::unexpected{ Error::Canceled }; }
                throw;
            }
        }

        /**
         * @brief Retain probe ownership before initial suspension and normalize bridge cancellation.
         * @param self Owned client state.
         * @param epoch Issue-time Stop epoch.
         * @param lifetime Optional owned pool loan.
         * @return Best-effort connection health or a lifecycle failure.
         */
        static auto aliveTask(
            std::shared_ptr<ClientImpl> self,
            std::uint64_t epoch,
            std::shared_ptr<void> lifetime
        ) -> Task<Result<bool>> {
            if (self->m_initialization_error) { co_return std::unexpected{ *self->m_initialization_error }; }
            if (self->m_terminal.load() || self->m_stop_epoch.load() != epoch) { co_return std::unexpected{ Error::Canceled }; }
            auto token = self->m_runtime->StopToken();
            if (!token) { co_return std::unexpected{ token.error() }; }
            try { co_return co_await ToTask(self->m_executor, alive(self, epoch, *token)); }
            catch (std::system_error const& error) {
                if (error.code() == Error::Canceled) { co_return std::unexpected{ Error::Canceled }; }
                throw;
            }
        }

        /**
         * @brief Retire idle monitoring, probe socket readability without consuming bytes, then restore monitoring.
         * @param self Owned client state on its strand.
         * @param epoch Issue-time Stop epoch.
         * @param token Current runtime generation.
         * @return True only when an established socket remains quiet for the bounded probe interval.
         * @note Readability on an idle HTTP connection means EOF, failure, or unsolicited bytes: all retire it.
         */
        static auto alive(
            std::shared_ptr<ClientImpl> self,
            std::uint64_t epoch,
            std::stop_token token
        ) -> asio::awaitable<Result<bool>> {
            co_await asio::this_coro::throw_if_cancelled(false);
            auto const parent = co_await asio::this_coro::cancellation_state;
            if (self->m_busy) { co_return std::unexpected{ Error::Busy }; }
            if (self->m_terminal.load() || self->m_stop_epoch.load() != epoch || token.stop_requested()
                || parent.cancelled() != asio::cancellation_type::none) { co_return std::unexpected{ Error::Canceled }; }
            self->m_busy = true;
            SendScope scope{ *self };
            if (self->m_active_epoch != epoch) { self->closeConnection(); }
            self->m_active_epoch = epoch;
            self->linkRuntime(token);
            co_await self->stopIdleWatch();
            if (self->isCanceled(epoch) || parent.cancelled() != asio::cancellation_type::none) {
                co_return std::unexpected{ Error::Canceled };
            }
            if (!self->m_connection) { co_return false; }
            auto connection = self->m_connection;
            auto ready = co_await connection->WaitForPeerData(std::chrono::milliseconds{ 1 });
            if (self->isCanceled(epoch) || parent.cancelled() != asio::cancellation_type::none) {
                co_return std::unexpected{ Error::Canceled };
            }
            if (!ready && ready.error() == Error::ReadTimeout) {
                self->watchIdleConnection();
                scope.succeeded = true;
                co_return true;
            }
            if (!ready && ready.error() == Error::Canceled) { co_return std::unexpected{ Error::Canceled }; }
            co_return false;
        }

        /**
         * @brief Resolve and connect lazily under one deadline, retaining the transport through initialization.
         * @param epoch Current Send cancellation epoch.
         * @param origin Effective destination for this hop, retained by the awaiting Send frame.
         * @param timeout Remaining establishment budget, absent to disable automatic timing.
         * @return Success or classified DNS/connect/deadline/cancellation failure.
         */
        auto connect(
            std::uint64_t epoch,
            Url const& origin,
            std::optional<Duration> timeout
        ) -> asio::awaitable<Result<void>> {
            auto deadline = std::make_shared<ConnectDeadline>(m_executor);
            m_connect_deadline = deadline;
            if (timeout) {
                if (*timeout <= Duration::zero()) { co_return std::unexpected{ Error::ConnectionTimeout }; }
                deadline->timer.expires_after(*timeout);
                deadline->timer.async_wait([weak = weak_from_this(), deadline](asio::error_code error) {
                    if (!error && !deadline->finished) {
                        deadline->expired = true;
                        if (auto self = weak.lock()) { self->cancelNetwork(); }
                    }
                });
            }
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
         * @brief Invoke a stable Send-owned callback inside a Task so even synchronous exceptions are contained.
         * @tparam F Callback returning Task<Result<T>>.
         * @tparam Args Owned arguments or explicit reference wrappers borrowing the awaiting frame.
         * @param callback Borrowed callable retained by the Send until the complete Task tree drains.
         * @param args Owned arguments, forwarded only when the Task starts.
         * @return Lazy callback result.
         */
        template <typename F, typename... Args>
        static auto callbackTask(F& callback, Args... args) -> std::invoke_result_t<F&, Args...> {
            co_return co_await std::invoke(callback, std::move(args)...);
        }

        /**
         * @brief Link one callback await to client Stop, inherited cancellation, runtime workers, and its budget.
         * @tparam T Owned callback result, including void.
         * @param task Owned callback Task whose callable remains in the awaiting Send frame.
         * @param remaining Remaining transfer budget, or nullopt for Never.
         * @param timeout_error Contextual timeout classification.
         * @return Result after all callback frames and nested operations have drained.
         */
        template <typename T>
        auto runCallback(
            Task<Result<T>> task,
            std::optional<Duration> remaining,
            Error timeout_error
        ) -> asio::awaitable<Result<T>> {
            if (isCanceled(m_active_epoch)) { co_return std::unexpected{ Error::Canceled }; }
            auto state = std::make_shared<ClientCallback>(m_executor);
            m_callback = state;
            /**
             * @brief Clear active cancellation ownership even on unexpected bridge allocation failures.
             */
            struct CallbackLease {
                std::shared_ptr<ClientCallback>& active; ///< Slot owned by the awaiting ClientImpl.
                /**
                 * @brief Release only after the callback root has drained.
                 */
                ~CallbackLease() { active.reset(); }
            } lease{ m_callback };
            co_return co_await state->Run(std::move(task), remaining, timeout_error, m_runtime);
        }

        /**
         * @brief Await optional progress under the same budget as its associated transfer phase.
         * @param callbacks Borrowed Send-owned callback identities.
         * @param progress Owned decoded-byte sample.
         * @param remaining Remaining transfer budget.
         * @param timeout_error Direction-specific timeout classification.
         * @return Success when absent or accepted, otherwise a terminal transfer error.
         */
        auto reportProgress(
            client::TransferCallbacks& callbacks,
            client::TransferProgress progress,
            std::optional<Duration> remaining,
            Error timeout_error
        ) -> asio::awaitable<Result<void>> {
            if (!callbacks.on_progress) { co_return Result<void>{}; }
            co_return co_await runCallback(callbackTask(callbacks.on_progress, progress), remaining, timeout_error);
        }

        /**
         * @brief Compute a positive provider request size that also fits exact chunk framing when needed.
         * @param chunked Whether hexadecimal length and CRLF delimiters consume write capacity.
         * @return Maximum decoded segment, or zero when no framed payload can fit.
         */
        auto providerChunkSize(bool chunked) const noexcept -> std::size_t {
            auto const wire_limit = m_connection->MaxWriteSize();
            auto count = std::min(m_options.upload_chunk_bytes, wire_limit);
            if (!chunked) { return count; }
            while (count != 0) {
                std::size_t overhead{ 4 };
                auto value = count;
                do { ++overhead; value /= 16; } while (value != 0);
                if (overhead >= wire_limit) { return 0; }
                if (count <= wire_limit - overhead) { return count; }
                count = wire_limit - overhead;
            }
            return 0;
        }

        /**
         * @brief Pull and transmit bounded provider chunks without buffering or consuming a source before upload permission.
         * @param request Send-owned provider, retained unchanged for redirect replay from offset zero.
         * @param epoch Current request cancellation epoch.
         * @param deadlines Send-owned timeout policy, also bounding provider and progress awaits.
         * @param until Fixed write phase/overall deadline, absent for Step/Never.
         * @param callbacks Borrowed Send-owned asynchronous progress.
         * @param hop Zero-based redirect index.
         * @return Complete fixed/chunked upload, provider error, premature EOF, size violation, or I/O/cancellation.
         */
        auto writeProvider(
            detail::PreparedClientRequest& request,
            std::uint64_t epoch,
            ClientDeadlines const& deadlines,
            std::optional<Clock::time_point> until,
            client::TransferCallbacks& callbacks,
            std::size_t hop
        ) -> asio::awaitable<Result<void>> {
            auto connection = m_connection;
            auto const total = request.request.ContentLength();
            bool const chunked = request.head.body_kind == detail::BodyKind::Chunked;
            auto const capacity = providerChunkSize(chunked);
            if (capacity == 0) { co_return std::unexpected{ Error::BodyTooLarge }; }
            auto reported = co_await reportProgress(callbacks, { client::TransferDirection::Upload, 0, total, hop },
                deadlines.Remaining(until), Error::WriteTimeout);
            if (!reported) { co_return reported; }
            detail::ChunkEncoder encoder;
            std::uint64_t offset{};
            while (!total || offset != *total) {
                auto const count = total ? static_cast<std::size_t>(std::min<std::uint64_t>(capacity, *total - offset)) : capacity;
                auto block = co_await runCallback(callbackTask(request.request.m_impl->provider, offset, count),
                    deadlines.Remaining(until), Error::WriteTimeout);
                if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (!block) { co_return std::unexpected{ block.error() }; }
                if (block->size() > count || block->size() > m_options.upload_body_bytes - offset) {
                    co_return std::unexpected{ Error::BodyTooLarge };
                }
                if (block->empty()) {
                    if (total) { co_return std::unexpected{ Error::InvalidRequest }; }
                    break;
                }
                auto const size = block->size();
                auto wire = chunked ? encoder.Encode(*block) : Result<std::string>{ std::move(*block) };
                if (!wire) { co_return std::unexpected{ wire.error() }; }
                auto written = co_await connection->WriteAll(std::move(*wire), deadlines.Remaining(until));
                if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (!written) { co_return written; }
                offset += size;
                reported = co_await reportProgress(callbacks, { client::TransferDirection::Upload, offset, total, hop },
                    deadlines.Remaining(until), Error::WriteTimeout);
                if (!reported) { co_return reported; }
            }
            if (chunked) {
                auto terminal = encoder.Finish();
                if (!terminal) { co_return std::unexpected{ terminal.error() }; }
                auto written = co_await connection->WriteAll(std::move(*terminal), deadlines.Remaining(until));
                if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (!written) { co_return written; }
            }
            co_return co_await reportProgress(callbacks, { client::TransferDirection::Upload, offset, total, hop, true },
                deadlines.Remaining(until), Error::WriteTimeout);
        }

        /**
         * @brief Transfer an owned buffered body once, retaining replay storage for configured redirects.
         * @param request Send-owned prepared request, alive throughout this coroutine.
         * @param epoch Current request cancellation epoch.
         * @param deadlines Send-owned timeout policy.
         * @param until Fixed write phase/overall deadline, absent for Step/Never.
         * @param callbacks Borrowed Send-owned callbacks, awaited before another segment is sent.
         * @param hop Zero-based redirect index for upload progress.
         * @return Complete upload or a classified write/callback/cancellation failure.
         */
        auto writeBody(
            detail::PreparedClientRequest& request,
            std::uint64_t epoch,
            ClientDeadlines const& deadlines,
            std::optional<Clock::time_point> until,
            client::TransferCallbacks& callbacks,
            std::size_t hop
        ) -> asio::awaitable<Result<void>> {
            if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
            if (request.request.IsStreaming()) {
                co_return co_await writeProvider(request, epoch, deadlines, until, callbacks, hop);
            }
            auto connection = m_connection;
            auto bytes = m_options.max_redirects == 0 ? std::move(request.request.m_impl->body)
                : std::string{ request.request.Body() };
            if (!callbacks.on_progress) {
                auto written = co_await connection->WriteAll(std::move(bytes), deadlines.Remaining(until));
                if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                co_return written;
            }
            auto const total = static_cast<std::uint64_t>(bytes.size());
            auto reported = co_await reportProgress(callbacks, { client::TransferDirection::Upload, 0, total, hop },
                deadlines.Remaining(until), Error::WriteTimeout);
            if (!reported) { co_return reported; }
            std::size_t offset{};
            while (offset != bytes.size()) {
                auto const count = std::min(m_options.upload_chunk_bytes, bytes.size() - offset);
                auto written = co_await connection->WriteAll(bytes.substr(offset, count), deadlines.Remaining(until));
                if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (!written) { co_return written; }
                offset += count;
                reported = co_await reportProgress(callbacks, { client::TransferDirection::Upload, offset, total, hop },
                    deadlines.Remaining(until), Error::WriteTimeout);
                if (!reported) { co_return reported; }
            }
            co_return co_await reportProgress(callbacks, { client::TransferDirection::Upload, total, total, hop, true },
                deadlines.Remaining(until), Error::WriteTimeout);
        }

        /**
         * @brief Negotiate an upload and consume one final response with bounded awaited receivers and callbacks.
         * @param request Prepared request, retaining replayable upload storage.
         * @param epoch Current request cancellation epoch.
         * @param deadlines Borrowed Send-owned budget, retained through asynchronous reads and callbacks.
         * @param callbacks Owned by Send; on_body disables aggregate response buffering.
         * @param current Effective hop URL for deciding whether to suppress an intermediate redirect body.
         * @param hop Zero-based redirect index.
         * @return Immutable complete final metadata or a classified transport/protocol/callback failure.
         */
        auto readResponse(
            detail::PreparedClientRequest& request,
            std::uint64_t epoch,
            ClientDeadlines const& deadlines,
            client::TransferCallbacks& callbacks,
            Url const& current,
            std::size_t hop
        ) -> asio::awaitable<Result<client::Response>> {
            detail::ResponseParser parser{ request.request.Method().value_or(Method::Get),
                detail::client_parser_limits(m_options), true };
            auto until = deadlines.Until(m_options.read_timeout);
            auto const continue_until = Clock::now() + m_options.expect_continue_timeout;
            auto connection = m_connection;
            std::string input, body;
            std::size_t offset{};
            std::uint64_t received{};
            std::optional<std::uint64_t> total;
            std::optional<client::Response> head_snapshot;
            bool eof{};
            bool body_pending = request.expect_continue;
            bool body_suppressed{};
            bool discard_body{};
            for (;;) {
                if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                auto bytes = std::span{ input.data(), input.size() }.subspan(offset);
                auto step = parser.Feed(bytes.first(std::min(bytes.size(), m_options.response_chunk_bytes)));
                offset += step.consumed;
                if (step.event == detail::ParseEvent::NeedMore && eof && offset == input.size()) { step = parser.Finish(); }
                bool send_body{};
                switch (step.event) {
                case detail::ParseEvent::Informational:
                    if (parser.Head().status == Status::SwitchingProtocols) { co_return std::unexpected{ Error::ProtocolError }; }
                    send_body = body_pending && parser.Head().status == Status::Continue;
                    break;
                case detail::ParseEvent::HeadersComplete: {
                    if (body_pending) { body_pending = false; body_suppressed = true; }
                    if (unsupportedEncoding(parser.Head().headers)) { co_return std::unexpected{ Error::UnsupportedContentEncoding }; }
                    auto destination = detail::client_redirect_target(current, parser.Head().status, parser.Head().headers,
                        m_options.max_redirects, hop);
                    if (!destination) { co_return std::unexpected{ destination.error() }; }
                    discard_body = destination->has_value();
                    if (discard_body) { break; }
                    if (parser.Framing().kind == detail::BodyKind::ContentLength) { total = parser.Framing().content_length; }
                    else if (parser.Framing().kind == detail::BodyKind::None) { total = 0; }
                    if (callbacks.on_headers) {
                        head_snapshot.emplace(detail::ClientResponseAccess::Create(parser.Head(), {}, {}, false, m_remote, m_local));
                        auto accepted = co_await runCallback(callbackTask(callbacks.on_headers, std::cref(*head_snapshot)),
                            deadlines.Remaining(until), Error::ReadTimeout);
                        if (!accepted) { co_return std::unexpected{ accepted.error() }; }
                    }
                    auto reported = co_await reportProgress(callbacks,
                        { client::TransferDirection::Download, 0, total, hop }, deadlines.Remaining(until), Error::ReadTimeout);
                    if (!reported) { co_return std::unexpected{ reported.error() }; }
                    break;
                }
                case detail::ParseEvent::Body:
                    if (!discard_body) {
                        if (callbacks.on_body) {
                            auto accepted = co_await runCallback(callbackTask(callbacks.on_body,
                                std::string{ step.data.data(), step.data.size() }), deadlines.Remaining(until), Error::ReadTimeout);
                            if (!accepted) { co_return std::unexpected{ accepted.error() }; }
                        } else { body.append(step.data.data(), step.data.size()); }
                        received += step.data.size();
                        auto reported = co_await reportProgress(callbacks,
                            { client::TransferDirection::Download, received, total, hop }, deadlines.Remaining(until), Error::ReadTimeout);
                        if (!reported) { co_return std::unexpected{ reported.error() }; }
                    }
                    break;
                case detail::ParseEvent::Failed:
                    co_return std::unexpected{ step.failure->error };
                case detail::ParseEvent::Complete: {
                    if (offset != input.size()) { co_return std::unexpected{ Error::ProtocolError }; }
                    if (!discard_body) {
                        auto reported = co_await reportProgress(callbacks,
                            { client::TransferDirection::Download, received, total, hop, true }, deadlines.Remaining(until), Error::ReadTimeout);
                        if (!reported) { co_return std::unexpected{ reported.error() }; }
                    }
                    bool const keep_alive = request.head.keep_alive && parser.Framing().keep_alive && !eof && !body_suppressed;
                    co_return detail::ClientResponseAccess::Create(parser.Head(), std::move(body), parser.Trailers(),
                        keep_alive, m_remote, m_local);
                }
                case detail::ParseEvent::NeedMore: {
                    if (offset != input.size()) { break; }
                    auto remaining = deadlines.Remaining(until);
                    if (remaining && *remaining <= Duration::zero()) { co_return std::unexpected{ Error::ReadTimeout }; }
                    if (body_pending) {
                        auto const gap = continue_until - Clock::now();
                        if (gap <= Duration::zero()) { send_body = true; break; }
                        auto const read_until = remaining ? std::optional{ Clock::now() + *remaining } : std::nullopt;
                        bool const fallback_wait = !remaining || gap < *remaining;
                        auto ready = co_await connection->WaitForPeerData(fallback_wait ? gap : *remaining);
                        if (isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                        if (!ready) {
                            if (ready.error() == Error::ReadTimeout && fallback_wait && (!read_until || Clock::now() < *read_until)) {
                                send_body = true;
                                break;
                            }
                            co_return std::unexpected{ ready.error() };
                        }
                        if (read_until) {
                            remaining = *read_until - Clock::now();
                            if (*remaining <= Duration::zero()) { co_return std::unexpected{ Error::ReadTimeout }; }
                        }
                    }
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
                if (send_body) {
                    body_pending = false;
                    auto written = co_await writeBody(request, epoch, deadlines, deadlines.Until(m_options.write_timeout), callbacks, hop);
                    if (!written) { co_return std::unexpected{ written.error() }; }
                    until = deadlines.Until(m_options.read_timeout);
                }
            }
        }

        /**
         * @brief Run a bounded redirect chain on the client strand without admitting overlapping Sends.
         * @param self Owned implementation for the entire operation.
         * @param request Owned outgoing data.
         * @param callbacks Owned asynchronous receivers/progress, retained through all redirect hops.
         * @param headers Issue-time fallback fields, isolated from later configuration changes.
         * @param epoch Issue-time Stop epoch.
         * @param token Client runtime generation cancellation.
         * @return Complete response or recoverable validation/I/O/lifecycle failure.
         */
        static auto send(
            std::shared_ptr<ClientImpl> self,
            client::Request request,
            client::TransferCallbacks callbacks,
            Headers headers,
            std::uint64_t epoch,
            std::stop_token token
        ) -> asio::awaitable<Result<client::Response>> {
            co_await asio::this_coro::throw_if_cancelled(false);
            if (self->m_busy) { co_return std::unexpected{ Error::Busy }; }
            if (self->m_terminal.load() || self->m_stop_epoch.load() != epoch || token.stop_requested()) { co_return std::unexpected{ Error::Canceled }; }
            auto options = self->m_options;
            options.default_headers = std::move(headers);
            auto prepared = detail::prepare_client_request(std::move(request), *self->m_origin, options);
            if (!prepared) { co_return std::unexpected{ prepared.error() }; }
            auto current = Url::Resolve(*self->m_origin, prepared->target == "*" ? "/" : prepared->target);
            if (!current) { co_return std::unexpected{ current.error() }; }
            self->m_busy = true;
            SendScope scope{ *self };
            ClientDeadlines deadlines{ options };
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
                    auto connected = co_await self->connect(epoch, *current,
                        deadlines.Remaining(deadlines.Until(options.connect_timeout)));
                    self->retireConnectDeadline();
                    if (!connected) { co_return std::unexpected{ connected.error() }; }
                }
                auto const until = deadlines.Until(options.write_timeout);
                auto connection = self->m_connection;
                auto written = co_await connection->WriteAllWithProgress(prepared->head.bytes, deadlines.Remaining(until));
                if (self->isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (reused && written.error == Error::ConnectionClosed && written.bytes_transferred == 0) {
                    self->closeConnection();
                    auto connected = co_await self->connect(epoch, *current,
                        deadlines.Remaining(deadlines.Until(options.connect_timeout)));
                    self->retireConnectDeadline();
                    if (!connected) { co_return std::unexpected{ connected.error() }; }
                    connection = self->m_connection;
                    written = co_await connection->WriteAllWithProgress(prepared->head.bytes, deadlines.Remaining(until));
                }
                if (self->isCanceled(epoch)) { co_return std::unexpected{ Error::Canceled }; }
                if (written.error) { co_return std::unexpected{ *written.error }; }
                if (!prepared->expect_continue) {
                    auto body = co_await self->writeBody(*prepared, epoch, deadlines, until, callbacks, followed);
                    if (!body) { co_return std::unexpected{ body.error() }; }
                }
                auto response = co_await self->readResponse(*prepared, epoch, deadlines, callbacks, *current, followed);
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
