/**
 * @file server.cppm
 * @brief Provide the HTTP server: listening, the accept loops, session tracking, and shutdown.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:server;

import std;
import :config;
import :logging;
import :async_event;
import :when_all;
import :form_data;
import :trusted_proxies;
import :router;
import :reverse_proxy;
import :ws_forward;
import :session;

namespace mhttp {

    class ServerImpl;

} // namespace mhttp

export namespace mhttp {

    /**
     * @brief Asynchronous HTTP/1.1 server built on Boost.Asio coroutines.
     *
     * Every accepted connection runs on its own strand derived from the server strand, so
     * connections progress in parallel while each connection stays serialized.
     *
     * Configuration phase (before Run()/AsyncRun(), from one thread): Listen(), GetRouter(), and
     * SetTrustedProxies(). Any time, from any thread: timeouts, limits, form-data config, logger,
     * LocalEndpoint(), IsOpen(), Stop(), and AsyncStop(). One run cycle may be active at a time;
     * a second concurrent Run() reports net::error::already_started. Routes are cleared after
     * a stop drains, so register them again before re-running the same server.
     */
    class HttpServer {
    private:
        // Shared so in-flight coroutines keep the implementation alive after ~HttpServer().
        std::shared_ptr<ServerImpl> m_impl; ///< Server implementation.

    public:
        /**
         * @brief Create a server whose operations run on an io_context.
         * @param ioc Execution context; some thread must run it.
         */
        explicit HttpServer(net::io_context& ioc);

        /**
         * @brief Create a server whose operations run on an executor.
         * @param ex Executor from which the server strand is derived.
         */
        explicit HttpServer(net::any_io_executor const& ex);

        /**
         * @brief Request a stop without waiting for it.
         * @note Waiting could deadlock on an I/O thread or when the io_context no longer runs.
         *       Wait for Stop() or Run() before destruction for deterministic cleanup.
         */
        ~HttpServer();

        HttpServer(HttpServer const&) = delete;
        auto operator=(HttpServer const&) -> HttpServer& = delete;

        /**
         * @brief Bind to a local address and start listening.
         * @param host Address to bind, such as "127.0.0.1" or "0.0.0.0".
         * @param port Port to bind; 0 asks the OS for an ephemeral port (see LocalEndpoint()).
         * @return Success, or the resolve/open/bind/listen error.
         * @note Configuration phase only.
         */
        [[nodiscard]] auto Listen(std::string_view host, std::uint16_t port) -> Result<void>;

        /**
         * @brief Bind to all IPv4 interfaces and start listening.
         * @param port Port to bind; 0 asks the OS for an ephemeral port.
         * @return Success, or the resolve/open/bind/listen error.
         * @note Configuration phase only.
         */
        [[nodiscard]] auto Listen(std::uint16_t port) -> Result<void>;

        /**
         * @brief Start accepting connections.
         * @return Future that becomes ready after the server stopped and every session drained;
         *         it holds already_started for a concurrent run, or the first accept-loop error.
         * @note Does not run the executor; some thread must run it.
         */
        auto Run() -> std::future<boost::system::error_code>;

        /**
         * @brief Start accepting connections and complete after the server fully stopped.
         * @param ec Receives already_started or the first accept-loop error; left untouched on
         *           a normal stop, so clear it beforehand.
         */
        auto AsyncRun(boost::system::error_code& ec) -> net::awaitable<void>;

        /**
         * @brief Stop accepting, abort every session, and wait for the drain.
         * @return Future that becomes ready once the server fully stopped.
         * @note Do not block on the future from an I/O thread of this server; use AsyncStop().
         */
        auto Stop() -> std::future<void>;

        /**
         * @brief Stop accepting, abort every session, and complete once the server fully stopped.
         */
        auto AsyncStop() -> net::awaitable<void>;

        /**
         * @brief Borrow the router to register handlers, middleware, and static mounts.
         * @return Router of this server.
         * @note Configuration phase only: the router is not synchronized.
         */
        [[nodiscard]] auto GetRouter() -> Router&;

        /**
         * @brief Forward a literal prefix and its descendants to one HTTP upstream.
         * @param prefix Prefix such as "/api"; also registers the bare path.
         * @param upstream HTTP origin with an optional base path; no query or fragment.
         * @param config Streaming, path rewrite, timeout, and upstream pool settings.
         * @throws std::invalid_argument For invalid settings.
         * @note Configuration phase only; HTTP middleware applies. Supports GET, HEAD, POST,
         *       PUT, PATCH, DELETE, OPTIONS, and TRACE. WebSockets use SetWsForward separately.
         */
        auto SetReverseProxy(
            std::string prefix,
            std::string_view upstream,
            ReverseProxyConfig config = {}
        ) -> void;

        /**
         * @brief Forward WebSocket upgrades at a literal prefix to one ws:// upstream.
         * @param prefix Prefix such as "/socket"; also registers the bare path.
         * @param upstream WebSocket origin with an optional base path; no query or fragment.
         * @param config Path, Host, handshake deadline, and complete-message limits.
         * @throws std::invalid_argument For invalid settings.
         * @note Configuration phase only. Upstream negotiation finishes before downstream 101.
         *       Like local WebSocket routes, matched upgrades bypass HTTP middleware.
         */
        auto SetWsForward(
            std::string prefix,
            std::string_view upstream,
            WsForwardConfig config = {}
        ) -> void;

        /**
         * @brief Replace the multipart/form-data limits for subsequent requests.
         * @param params Save directory, per-part size limit, field limit, and cleanup policy.
         */
        auto SetFormDataConfig(FormData::Param const& params) -> void;

        /**
         * @brief Configure which proxies may vouch for clients via X-Forwarded-For.
         *
         * By default no proxy is trusted and Request::GetClientIp() returns the TCP peer; the
         * header is client-controlled, so trusting it blindly lets anyone spoof their address.
         *
         * @param cidrs CIDRs ("10.0.0.0/8") or single addresses; empty clears the setting.
         * @throws std::invalid_argument If an entry does not parse.
         * @note Configuration phase only.
         */
        auto SetTrustedProxies(std::vector<std::string> const& cidrs) -> void;

        /**
         * @brief Return the endpoint cached by Listen().
         * @return Bound endpoint, including the OS-assigned port.
         */
        [[nodiscard]] auto LocalEndpoint() const -> tcp::endpoint const&;

        /**
         * @brief Report whether the acceptor is open.
         * @return Snapshot of the acceptor state.
         */
        [[nodiscard]] auto IsOpen() const -> bool;

        /**
         * @brief Set the timeout for each read, including the idle wait between requests.
         * @param timeout New timeout; applies to subsequent reads.
         */
        auto SetReadTimeout(std::chrono::steady_clock::duration timeout) -> void;

        /**
         * @brief Set the timeout for each response write.
         * @param timeout New timeout; applies to subsequent writes.
         */
        auto SetWriteTimeout(std::chrono::steady_clock::duration timeout) -> void;

        /**
         * @brief Return the read timeout.
         * @return Current read timeout.
         */
        [[nodiscard]] auto ReadTimeout() const -> std::chrono::steady_clock::duration;

        /**
         * @brief Return the write timeout.
         * @return Current write timeout.
         */
        [[nodiscard]] auto WriteTimeout() const -> std::chrono::steady_clock::duration;

        /**
         * @brief Set the maximum request header size.
         * @param limit Limit in bytes; applies to subsequent requests.
         */
        auto SetHeaderLimit(std::uint32_t limit) -> void;

        /**
         * @brief Set the maximum request body and WebSocket message size.
         * @param limit Limit in bytes; applies to subsequent requests and new WebSocket sessions.
         */
        auto SetBodyLimit(std::uint64_t limit) -> void;

        /**
         * @brief Return the active logger.
         * @return Custom logger when set, otherwise the default "mhttp.server" console logger.
         */
        [[nodiscard]] auto Logger() const -> std::shared_ptr<spdlog::logger>;

        /**
         * @brief Replace the logger.
         * @param logger New logger; nullptr restores the default console logger.
         */
        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void;
    };

} // namespace mhttp

namespace mhttp {

    /**
     * @brief Classify accept errors that only affect one pending connection.
     * @param ec Error from async_accept.
     * @return True when the accept loop should keep going.
     */
    auto is_transient_accept_error(boost::system::error_code const& ec) -> bool {
        return ec == boost::system::errc::connection_aborted || ec == boost::system::errc::connection_reset || ec == boost::system::errc::interrupted || ec == boost::system::errc::operation_would_block || ec == boost::system::errc::resource_unavailable_try_again;
    }

    /**
     * @brief Server state machine: acceptor, accept loops, session registry, and shutdown drain.
     *
     * Strand topology (see httplib-src/THREAD_MODEL.md):
     * - m_strand runs the accept loops, session registration/removal, AsyncRun, and AsyncStop.
     * - Each session runs on its own strand made from m_strand's inner executor.
     */
    class ServerImpl : public std::enable_shared_from_this<ServerImpl> {
    private:
        static constexpr int ACCEPTOR_COUNT{ 32 };               ///< Concurrent async_accept operations.

        net::strand<net::any_io_executor> m_strand;              ///< Serializes server-level state.
        std::shared_ptr<ServerContext> m_context;                ///< State shared with sessions.
        tcp::acceptor m_acceptor;                                ///< Listening socket.
        tcp::endpoint m_local_endpoint;                          ///< Endpoint cached by Listen().
        std::unordered_set<std::shared_ptr<Session>> m_sessions; ///< Live sessions; only on m_strand.
        util::AsyncEvent m_session_event;                        ///< Notified when m_sessions becomes empty.
        util::AsyncEvent m_stop_event;                           ///< Closed when a run cycle ends.
        std::atomic<bool> m_running{ false };                    ///< A run cycle is active.

    public:
        /**
         * @brief Create an idle server on a new strand.
         * @param ex Executor from which the strand is derived.
         */
        explicit ServerImpl(
            net::any_io_executor const& ex
        ) : m_strand{ net::make_strand(ex) }, m_context{ std::make_shared<ServerContext>() }, m_acceptor{ m_strand }, m_session_event{ m_strand }, m_stop_event{ m_strand } {}

        /**
         * @brief Borrow the state shared with sessions.
         * @return Server context.
         */
        auto Context() -> ServerContext& {
            return *m_context;
        }

        /**
         * @brief Return the executor shared by server-owned upstream pools.
         */
        auto BaseExecutor() const -> net::any_io_executor {
            return m_strand.get_inner_executor();
        }

        /**
         * @brief Resolve, open, bind, and listen.
         * @param host Address to bind.
         * @param port Port to bind.
         * @return Success, or the first failing step's error; the acceptor is closed on failure.
         */
        auto Listen(std::string_view host, std::uint16_t port) -> Result<void> {
            boost::system::error_code ec;
            auto const fail{ [this](boost::system::error_code error) -> Result<void> {
                boost::system::error_code ignored;
                m_acceptor.close(ignored);
                return std::unexpected{ error };
            } };

            tcp::resolver resolver{ m_strand };
            auto const results{ resolver.resolve(host, std::to_string(port), ec) };
            if (ec) {
                return fail(ec);
            }
            tcp::endpoint const endpoint{ *results.begin() };

            m_acceptor.open(endpoint.protocol(), ec);
            if (ec) {
                return fail(ec);
            }
#ifndef _WIN32
            // On Windows SO_REUSEADDR lets another process steal the port, so only enable it
            // elsewhere (where it just allows rebinding during TIME_WAIT).
            m_acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
            if (ec) {
                return fail(ec);
            }
#endif
            m_acceptor.bind(endpoint, ec);
            if (ec) {
                return fail(ec);
            }
            m_acceptor.listen(net::socket_base::max_listen_connections, ec);
            if (ec) {
                return fail(ec);
            }

            m_local_endpoint = m_acceptor.local_endpoint(ec);
            m_context->logger.GetLogger()->info("Http Server Listen on: [{}:{}]", m_local_endpoint.address().to_string(), m_local_endpoint.port());
            return {};
        }

        /**
         * @brief Run one cycle and report its result through a future.
         * @return Future holding the cycle's error code.
         */
        auto Run() -> std::future<boost::system::error_code> {
            return net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<boost::system::error_code> {
                    boost::system::error_code ec;
                    co_await self->AsyncRun(ec);
                    co_return ec;
                },
                net::use_future
            );
        }

        /**
         * @brief Run one cycle from any executor.
         * @param ec Receives already_started or the first accept-loop error.
         */
        auto AsyncRun(boost::system::error_code& ec) -> net::awaitable<void> {
            auto const self{ shared_from_this() };
            co_await net::co_spawn(m_strand, runOnStrand(ec), net::use_awaitable);
        }

        /**
         * @brief Stop and report completion through a future.
         * @return Future that becomes ready once the cycle ended.
         */
        auto Stop() -> std::future<void> {
            return net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<void> {
                    co_await self->AsyncStop();
                },
                net::use_future
            );
        }

        /**
         * @brief Stop from any executor and complete once the cycle ended.
         */
        auto AsyncStop() -> net::awaitable<void> {
            auto const self{ shared_from_this() };
            co_await net::co_spawn(m_strand, stopOnStrand(), net::use_awaitable);
        }

        /**
         * @brief Report whether the acceptor is open.
         * @return Snapshot of the acceptor state.
         */
        [[nodiscard]] auto IsOpen() const -> bool {
            return m_acceptor.is_open();
        }

        /**
         * @brief Return the endpoint cached by Listen().
         * @return Bound endpoint.
         */
        [[nodiscard]] auto LocalEndpoint() const -> tcp::endpoint const& {
            return m_local_endpoint;
        }

    private:
        /**
         * @brief Body of one run cycle; executes on m_strand.
         * @param ec Receives already_started or the first accept-loop error.
         */
        auto runOnStrand(boost::system::error_code& ec) -> net::awaitable<void> {
            if (m_running.exchange(true)) {
                ec = net::error::already_started;
                co_return;
            }
            // Reopen the events so the server can run again after a previous cycle closed them.
            m_stop_event.Reset();
            m_session_event.Reset();

            std::vector<net::awaitable<boost::system::error_code>> loops;
            loops.reserve(ACCEPTOR_COUNT);
            for (int i{ 0 }; i < ACCEPTOR_COUNT; ++i) {
                loops.push_back(acceptLoop());
            }
            auto const results{ co_await util::when_all(std::move(loops)) };

            // Accepting stopped, so no session can be added; wait for the live ones to finish.
            while (!m_sessions.empty()) {
                if (co_await m_session_event.Wait() != util::AsyncEvent::WaitResult::Notified) {
                    break;
                }
            }

            // No session is left, so nothing reads the router concurrently; clearing it also
            // releases whatever the handlers captured (often the server itself).
            route_table(m_context->router).Reset();
            m_running = false;
            m_session_event.Close();
            m_stop_event.Close();
            for (auto const& loop_ec : results) {
                // Cancelling the acceptor completes pending accepts with operation_aborted,
                // which is the normal stop path.
                if (loop_ec && loop_ec != net::error::operation_aborted) {
                    ec = loop_ec;
                    co_return;
                }
            }
        }

        /**
         * @brief Body of a stop request; executes on m_strand.
         */
        auto stopOnStrand() -> net::awaitable<void> {
            if (!m_running) {
                co_return;
            }
            boost::system::error_code ec;
            m_acceptor.cancel(ec);
            m_acceptor.close(ec);

            m_context->logger.GetLogger()->trace("[server] stopping, {} sessions remaining", m_sessions.size());
            for (auto const& session : m_sessions) {
                session->Abort();
            }
            // runOnStrand() closes m_stop_event when the cycle ends, waking every stopper.
            (void)co_await m_stop_event.Wait();
        }

        /**
         * @brief Accept connections until the acceptor fails or is closed; executes on m_strand.
         * @return The terminating error, operation_aborted on a normal stop.
         */
        auto acceptLoop() -> net::awaitable<boost::system::error_code> {
            using namespace std::chrono_literals;
            boost::system::error_code ec;
            for (;;) {
                // One strand per connection: its socket, I/O, and abort all serialize on it.
                auto strand{ net::make_strand(m_strand.get_inner_executor()) };
                tcp::socket socket{ strand };
                co_await m_acceptor.async_accept(socket, net::redirect_error(net::use_awaitable, ec));
                if (ec) {
                    if (ec == boost::system::errc::too_many_files_open || ec == boost::system::errc::too_many_files_open_in_system) {
                        ec = {};
                        net::steady_timer retry{ co_await net::this_coro::executor };
                        retry.expires_after(100ms);
                        co_await retry.async_wait(net::redirect_error(net::use_awaitable, ec));
                        if (!ec) {
                            continue;
                        }
                    } else if (is_transient_accept_error(ec)) {
                        // One failed accept must not permanently reduce the number of loops.
                        m_context->logger.GetLogger()->trace("accept transient error: {}", ec.message());
                        ec = {};
                        continue;
                    } else if (ec != net::error::operation_aborted) {
                        m_context->logger.GetLogger()->error("accept failed: {}", ec.message());
                    }
                    break;
                }

                boost::system::error_code option_ec;
                socket.set_option(tcp::no_delay(true), option_ec);
                spawnSession(std::move(socket), strand);
            }
            m_context->logger.GetLogger()->trace("accept loop ended: {}", ec.message());
            co_return ec;
        }

        /**
         * @brief Register and start a session for an accepted socket; executes on m_strand.
         * @param socket Accepted socket bound to `executor`.
         * @param executor The connection's strand.
         */
        auto spawnSession(tcp::socket socket, net::any_io_executor executor) -> void {
            boost::system::error_code ep_ec;
            auto const remote{ socket.remote_endpoint(ep_ec) };
            m_context->logger.GetLogger()->trace("accept new connection [{}:{}]", remote.address().to_string(), remote.port());

            auto session{ std::make_shared<Session>(executor, std::move(socket), m_context) };
            // Register synchronously before spawning: once the accept loops end, the drain in
            // runOnStrand() must see every accepted session.
            m_sessions.insert(session);

            net::co_spawn(executor, session->Run(), [self = shared_from_this(), session, remote](std::exception_ptr e) {
                // The completion runs on the connection strand; the registry lives on m_strand.
                net::dispatch(self->m_strand, [self, session, remote]() {
                    self->m_sessions.erase(session);
                    self->m_context->logger.GetLogger()->trace("close connection [{}:{}], {} sessions remaining", remote.address().to_string(), remote.port(), self->m_sessions.size());
                    if (self->m_sessions.empty()) {
                        self->m_session_event.NotifyAll();
                    }
                });
                if (!e) {
                    return;
                }
                try {
                    std::rethrow_exception(e);
                } catch (std::exception const& ex) {
                    self->m_context->logger.GetLogger()->error("session::Run() exception: {}", ex.what());
                } catch (...) {
                    self->m_context->logger.GetLogger()->error("session::Run() unknown exception");
                }
            });
        }
    };

    HttpServer::HttpServer(net::io_context& ioc) : HttpServer{ ioc.get_executor() } {}

    HttpServer::HttpServer(
        net::any_io_executor const& ex
    ) : m_impl{ std::make_shared<ServerImpl>(ex) } {}

    HttpServer::~HttpServer() {
        // The returned future is discarded on purpose; see the class contract.
        (void)Stop();
    }

    auto HttpServer::Listen(std::string_view host, std::uint16_t port) -> Result<void> {
        return m_impl->Listen(host, port);
    }

    auto HttpServer::Listen(std::uint16_t port) -> Result<void> {
        return Listen("0.0.0.0", port);
    }

    auto HttpServer::Run() -> std::future<boost::system::error_code> {
        return m_impl->Run();
    }

    auto HttpServer::AsyncRun(boost::system::error_code& ec) -> net::awaitable<void> {
        return m_impl->AsyncRun(ec);
    }

    auto HttpServer::Stop() -> std::future<void> {
        return m_impl->Stop();
    }

    auto HttpServer::AsyncStop() -> net::awaitable<void> {
        return m_impl->AsyncStop();
    }

    auto HttpServer::GetRouter() -> Router& {
        return m_impl->Context().router;
    }

    auto HttpServer::SetReverseProxy(
        std::string prefix,
        std::string_view upstream,
        ReverseProxyConfig config
    ) -> void {
        auto proxy{ std::make_shared<detail::ReverseProxy>(m_impl->BaseExecutor(), std::move(prefix), upstream, config) };
        auto const handler{ [proxy](Request& request, Response& response) -> net::awaitable<void> {
            co_await proxy->Run(request, response);
        } };
        auto const& base{ proxy->Prefix() };
        GetRouter().SetLazyHttpHandler<Verb::get, Verb::head, Verb::post, Verb::put, Verb::patch, Verb::delete_, Verb::options, Verb::trace>(base == "/" ? "/*" : base + "/*", handler);
        GetRouter().SetLazyHttpHandler<Verb::get, Verb::head, Verb::post, Verb::put, Verb::patch, Verb::delete_, Verb::options, Verb::trace>(base, handler);
    }

    auto HttpServer::SetWsForward(
        std::string prefix,
        std::string_view upstream,
        WsForwardConfig config
    ) -> void {
        auto forward{ std::make_shared<detail::WsForward>(std::move(prefix), upstream, config) };
        auto prepare{ [forward](Request& request) -> net::awaitable<std::expected<detail::WsHandlerEntry, Status>> {
            co_return co_await forward->Prepare(request);
        } };
        auto const& base{ forward->Prefix() };
        auto& table{ route_table(GetRouter()) };
        table.AddWsForward(base == "/" ? "/*" : base + "/*", prepare);
        table.AddWsForward(base, std::move(prepare));
    }

    auto HttpServer::SetFormDataConfig(FormData::Param const& params) -> void {
        m_impl->Context().form_data_params.store(std::make_shared<FormData::Param const>(params));
    }

    auto HttpServer::SetTrustedProxies(std::vector<std::string> const& cidrs) -> void {
        // An empty list becomes null: requests then skip X-Forwarded-For entirely.
        m_impl->Context().trusted_proxies.store(cidrs.empty() ? nullptr : std::make_shared<detail::TrustedProxies const>(cidrs));
    }

    auto HttpServer::LocalEndpoint() const -> tcp::endpoint const& {
        return m_impl->LocalEndpoint();
    }

    auto HttpServer::IsOpen() const -> bool {
        return m_impl->IsOpen();
    }

    auto HttpServer::SetReadTimeout(std::chrono::steady_clock::duration timeout) -> void {
        m_impl->Context().read_timeout.store(timeout);
    }

    auto HttpServer::SetWriteTimeout(std::chrono::steady_clock::duration timeout) -> void {
        m_impl->Context().write_timeout.store(timeout);
    }

    auto HttpServer::ReadTimeout() const -> std::chrono::steady_clock::duration {
        return m_impl->Context().read_timeout.load();
    }

    auto HttpServer::WriteTimeout() const -> std::chrono::steady_clock::duration {
        return m_impl->Context().write_timeout.load();
    }

    auto HttpServer::SetHeaderLimit(std::uint32_t limit) -> void {
        m_impl->Context().header_limit.store(limit);
    }

    auto HttpServer::SetBodyLimit(std::uint64_t limit) -> void {
        m_impl->Context().body_limit.store(limit);
    }

    auto HttpServer::Logger() const -> std::shared_ptr<spdlog::logger> {
        return m_impl->Context().logger.GetLogger();
    }

    auto HttpServer::SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
        m_impl->Context().logger.SetLogger(std::move(logger));
    }

} // namespace mhttp
