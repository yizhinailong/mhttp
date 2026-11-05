/**
 * @file client_pool.cppm
 * @brief Provide HttpClientPool: per-origin reuse of HttpClient connections with fair waiting.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

export module mhttp:client_pool;

import std;
import :config;
import :error;
import :strings;
import :logging;
import :async_event;
import :url;
import :tls;
import :client_impl;
import :client;

export namespace mhttp::client {

    /**
     * @brief HttpClientPool limits and the settings applied to every pooled client.
     *
     * `max_size == 0` stops creating connections (idle ones are still reused); `max_total == 0`
     * means no global limit.
     */
    struct PoolParams {
        std::size_t max_size{ 100 };                                                           ///< Connections per origin.
        std::size_t max_total{ 0 };                                                            ///< Connections overall; 0 = unlimited.
        std::chrono::steady_clock::duration idle_timeout{ std::chrono::seconds{ 60 } };        ///< Idle age before eviction; 0 = never.
        std::chrono::steady_clock::duration idle_check_interval{ std::chrono::seconds{ 60 } }; ///< Eviction period; 0 = 60 s.
        bool validate_on_borrow{ false };                                                      ///< Probe idle connections before lending.
        TimeoutPolicy timeout_policy{ TimeoutPolicy::Overall };                                ///< Client timeout policy.
        std::chrono::steady_clock::duration timeout{ std::chrono::seconds{ 30 } };             ///< Client timeout.
        int max_redirects{ 0 };                                                                ///< Client redirect limit.
        std::shared_ptr<TlsClientContext> tls_context;                                         ///< Shared HTTPS trust; null uses system roots.
    };

    /**
     * @brief Pool of HttpClient connections keyed by origin (port of httplib's http_client_pool).
     *
     * AsyncAcquire() lends a client through a ClientHandle; destroying or releasing the handle
     * returns it, unless it is still busy (see HttpClient::HasActiveSession()), in which case it
     * is closed. Waiters are served first come, first served. Every client gets its own strand
     * on the pool's executor; pool state lives on the pool strand. Stop the pool (or destroy it)
     * before stopping its executor.
     */
    class HttpClientPool {
    private:
        class Impl;

    public:
        /**
         * @brief Borrowed client; returns it to the pool on destruction.
         */
        class ClientHandle {
        private:
            std::weak_ptr<Impl> m_pool;           ///< Owning pool.
            std::unique_ptr<HttpClient> m_client; ///< Borrowed client; null on failure.
            boost::system::error_code m_error;    ///< Why no client is held.

            friend class HttpClientPool::Impl;

            ClientHandle(
                std::weak_ptr<Impl> pool,
                std::unique_ptr<HttpClient> client
            ) : m_pool{ std::move(pool) }, m_client{ std::move(client) } {}

        public:
            /**
             * @brief Create an empty handle carrying Error::InvalidHandle.
             */
            ClientHandle() : ClientHandle{ make_error_code(Error::InvalidHandle) } {}

            /**
             * @brief Create a failed handle.
             * @param ec Reason no client was lent.
             */
            explicit ClientHandle(boost::system::error_code ec) : m_error{ ec } {}

            ClientHandle(ClientHandle const&) = delete;
            auto operator=(ClientHandle const&) -> ClientHandle& = delete;

            ClientHandle(
                ClientHandle&& other
            ) noexcept : m_pool{ std::move(other.m_pool) }, m_client{ std::move(other.m_client) }, m_error{ other.m_error } {
                if (!other.m_error) {
                    other.m_error = make_error_code(Error::InvalidHandle);
                }
            }

            auto operator=(ClientHandle&& other) noexcept -> ClientHandle& {
                if (this != &other) {
                    Release();
                    m_pool = std::move(other.m_pool);
                    m_client = std::move(other.m_client);
                    m_error = other.m_error;
                    if (!other.m_error) {
                        other.m_error = make_error_code(Error::InvalidHandle);
                    }
                }
                return *this;
            }

            ~ClientHandle() {
                Release();
            }

            /**
             * @brief Return the borrowed client.
             * @return Client.
             * @throws boost::system::system_error GetError() when no client is held.
             */
            [[nodiscard]] auto Get() const -> HttpClient& {
                if (!m_client) {
                    throw boost::system::system_error{ m_error ? m_error : make_error_code(Error::InvalidHandle) };
                }
                return *m_client;
            }

            auto operator->() const -> HttpClient* {
                return &Get();
            }

            auto operator*() const -> HttpClient& {
                return Get();
            }

            explicit operator bool() const noexcept {
                return m_client != nullptr;
            }

            [[nodiscard]] auto HasError() const noexcept -> bool {
                return m_client == nullptr;
            }

            /**
             * @brief Return why no client is held.
             * @return Error, e.g. Error::AcquireTimeout; empty while a client is held.
             * @note Not named Error(): a member with that name would hide the Error enum here.
             */
            [[nodiscard]] auto GetError() const noexcept -> boost::system::error_code const& {
                return m_error;
            }

            /**
             * @brief Return the client to the pool now.
             */
            auto Release() -> void;
        };

        /**
         * @brief Connection counts of one origin.
         */
        struct PoolStats {
            std::size_t idle{ 0 };   ///< Connections waiting to be lent.
            std::size_t active{ 0 }; ///< Connections lent out.
        };

        static constexpr std::chrono::seconds DEFAULT_WAIT_TIMEOUT{ 3 }; ///< Default AsyncAcquire() wait.

    private:
        std::shared_ptr<Impl> m_impl; ///< Pool state; null after a move.

    public:
        /**
         * @brief Create and start a pool.
         * @param executor Executor for the pool strand and every client.
         * @param params Limits and client settings.
         */
        explicit HttpClientPool(net::any_io_executor const& executor, PoolParams params = {});

        /**
         * @brief Stop the pool; idle connections close once its executor runs the teardown.
         */
        ~HttpClientPool();

        HttpClientPool(HttpClientPool const&) = delete;
        auto operator=(HttpClientPool const&) -> HttpClientPool& = delete;
        HttpClientPool(HttpClientPool&& other) noexcept = default;
        auto operator=(HttpClientPool&& other) noexcept -> HttpClientPool& = default;

        /**
         * @brief Borrow a client for an origin, reusing an idle one or creating a new one.
         * @param host Host name or address.
         * @param port Port.
         * @param scheme Transport.
         * @param wait_timeout How long to wait at capacity; zero or negative fails immediately.
         * @return Handle with a client, or Error::AcquireTimeout / operation_canceled.
         */
        auto AsyncAcquire(
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain,
            std::chrono::steady_clock::duration wait_timeout = DEFAULT_WAIT_TIMEOUT
        ) -> net::awaitable<ClientHandle>;

        /**
         * @brief Borrow a client for the origin of a URL.
         * @param base_url "http://host[:port]..." or "https://...".
         * @param wait_timeout How long to wait at capacity.
         * @return Handle with a client, or Error::InvalidUrl / AcquireTimeout / operation_canceled.
         */
        auto AsyncAcquire(
            std::string_view base_url,
            std::chrono::steady_clock::duration wait_timeout = DEFAULT_WAIT_TIMEOUT
        ) -> net::awaitable<ClientHandle>;

        [[nodiscard]] auto Logger() const -> std::shared_ptr<spdlog::logger>;
        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void;

        /**
         * @brief Count lent connections; an atomic snapshot callable from any thread.
         * @return Connections lent out.
         */
        [[nodiscard]] auto ActiveCount() const -> std::size_t;
        [[nodiscard]] auto IdleCount() const -> std::size_t;
        [[nodiscard]] auto TotalCount() const -> std::size_t;

        /**
         * @brief Stop permanently: refuse new borrows, wake waiters, close idle connections.
         * @note Idempotent. Cleanup runs on the pool strand; it does not cancel requests in flight.
         */
        auto Stop() -> void;

        /**
         * @brief Read the counts of one origin on the pool strand.
         * @return Future ready once read; do not wait on it from the pool's I/O thread.
         */
        [[nodiscard]] auto Stats(
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        ) const -> std::future<PoolStats>;

        auto AsyncStats(
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        ) const -> net::awaitable<PoolStats>;
    };

} // namespace mhttp::client

namespace mhttp::client {

    /**
     * @brief State behind HttpClientPool; every member below is touched on m_strand only, except
     *        the atomics.
     */
    class HttpClientPool::Impl : public std::enable_shared_from_this<HttpClientPool::Impl> {
    private:
        /**
         * @brief Idle client and when it became idle.
         */
        struct PooledClient {
            std::unique_ptr<HttpClient> client;               ///< Idle client.
            std::chrono::steady_clock::time_point idle_since; ///< Time of return.
        };

        /**
         * @brief Connections of one origin.
         */
        struct RouteState {
            std::deque<PooledClient> idle; ///< Idle clients, oldest first.
            std::size_t active{ 0 };       ///< Lent clients.
            std::size_t validating{ 0 };   ///< Clients being probed by validate_on_borrow.
        };

        /**
         * @brief One coroutine waiting for capacity.
         */
        struct Waiter {
            util::AsyncEvent event; ///< Notified when capacity frees up; closed on stop.

            explicit Waiter(net::any_io_executor executor) : event{ std::move(executor) } {}
        };

        using WaiterList = std::deque<std::weak_ptr<Waiter>>;

        /**
         * @brief Result of one borrow attempt: a handle, or "no capacity, wait".
         */
        struct AcquireStep {
            ClientHandle handle; ///< Lent client when `wait` is false.
            bool wait{ false };  ///< No capacity: queue and wait.
        };

        net::any_io_executor m_executor;                      ///< Executor clients make their strands from.
        net::strand<net::any_io_executor> m_strand;           ///< Serializes pool state.
        net::steady_timer m_timer;                            ///< Idle eviction timer.
        PoolParams m_params;                                  ///< Limits and client settings.
        detail::LoggerHolder m_logger{ "mhttp.client_pool" }; ///< Replaceable logger.
        std::atomic<bool> m_running{ true };                  ///< Cleared by Stop().
        detail::StringMap<RouteState> m_routes;               ///< Connections per origin key.
        detail::StringMap<WaiterList> m_waiters;              ///< Waiters per origin key, FIFO.
        std::atomic<std::size_t> m_total{ 0 };                ///< Connections created and not destroyed.
        std::atomic<std::size_t> m_active{ 0 };               ///< Connections lent out.

    public:
        /**
         * @brief Create the pool state; call Start() once owned by a shared_ptr.
         * @param executor Executor for the pool strand and clients.
         * @param params Limits and client settings.
         */
        Impl(
            net::any_io_executor executor,
            PoolParams params
        ) : m_executor{ executor }, m_strand{ net::make_strand(executor) }, m_timer{ m_strand }, m_params{ std::move(params) } {}

        /**
         * @brief Start the idle eviction loop.
         */
        auto Start() -> void {
            net::co_spawn(m_strand, maintain(shared_from_this()), net::detached);
        }

        /**
         * @brief Stop permanently and tear down on the strand; idempotent.
         */
        auto Stop() -> void {
            if (!m_running.exchange(false)) {
                return;
            }
            net::post(m_strand, [self = shared_from_this()]() {
                self->m_timer.cancel();
                self->teardown();
            });
        }

        [[nodiscard]] auto Logger() const -> detail::LoggerHolder const& {
            return m_logger;
        }

        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
            m_logger.SetLogger(std::move(logger));
        }

        [[nodiscard]] auto ActiveCount() const -> std::size_t {
            return m_active.load();
        }

        [[nodiscard]] auto TotalCount() const -> std::size_t {
            return m_total.load();
        }

        [[nodiscard]] auto Strand() const -> net::strand<net::any_io_executor> {
            return m_strand;
        }

        /**
         * @brief Borrow a client (see HttpClientPool::AsyncAcquire()).
         * @return Handle.
         */
        auto AsyncAcquire(
            std::string host,
            std::uint16_t port,
            url::Scheme scheme,
            std::chrono::steady_clock::duration wait_timeout
        ) -> net::awaitable<ClientHandle> {
            co_return co_await net::co_spawn(m_strand, acquire(shared_from_this(), std::move(host), port, scheme, wait_timeout), net::use_awaitable);
        }

        /**
         * @brief Take a client back from any thread.
         * @param client Returned client.
         */
        auto Release(std::unique_ptr<HttpClient> client) -> void {
            net::post(m_strand, [self = shared_from_this(), client = std::move(client)]() mutable {
                self->releaseOnStrand(std::move(client));
            });
        }

        /**
         * @brief Read the counts of one origin on the strand.
         * @param key Origin key from url::make_url_value().
         * @return Counts.
         */
        auto AsyncStats(std::string key) -> net::awaitable<PoolStats> {
            co_return co_await net::co_spawn(
                m_strand,
                [self = shared_from_this(), key = std::move(key)]() -> net::awaitable<PoolStats> {
                    PoolStats stats;
                    if (auto const it{ self->m_routes.find(key) }; it != self->m_routes.end()) {
                        stats.idle = it->second.idle.size();
                        stats.active = it->second.active;
                    }
                    co_return stats;
                },
                net::use_awaitable
            );
        }

    private:
        /**
         * @brief Evict idle connections periodically until stopped.
         * @param self Keeps the pool alive while the loop runs.
         */
        static auto maintain(std::shared_ptr<Impl> self) -> net::awaitable<void> {
            auto const interval{ self->m_params.idle_check_interval > std::chrono::steady_clock::duration::zero() ? self->m_params.idle_check_interval : std::chrono::seconds{ 60 } };
            while (self->m_running.load()) {
                self->m_timer.expires_after(interval);
                boost::system::error_code ec;
                co_await self->m_timer.async_wait(net::redirect_error(net::use_awaitable, ec));
                if (!self->m_running.load()) {
                    break;
                }
                self->evictIdle();
            }
        }

        /**
         * @brief Borrow loop on the strand: reuse, create, or wait in FIFO order.
         * @return Handle.
         */
        static auto acquire(
            std::shared_ptr<Impl> self,
            std::string host,
            std::uint16_t port,
            url::Scheme scheme,
            std::chrono::steady_clock::duration wait_timeout
        ) -> net::awaitable<ClientHandle> {
            auto const canceled{ boost::system::errc::make_error_code(boost::system::errc::operation_canceled) };
            auto const key{ url::make_url_value(host, port, scheme) };
            auto const deadline{ std::chrono::steady_clock::now() + wait_timeout };
            std::shared_ptr<Waiter> node;
            bool serving{ false }; // woken by the pool, so allowed past the fairness check
            for (;;) {
                if (!self->m_running.load()) {
                    self->removeWaiter(key, node);
                    co_return ClientHandle{ canceled };
                }
                if (node && deadline <= std::chrono::steady_clock::now()) {
                    self->removeWaiter(key, node);
                    self->m_logger.GetLogger()->debug("client pool: acquire timed out for {}", key);
                    co_return ClientHandle{ make_error_code(Error::AcquireTimeout) };
                }
                auto step{ co_await self->acquireOrCreate(key, host, port, scheme, serving) };
                if (!step.wait) {
                    self->removeWaiter(key, node);
                    co_return std::move(step.handle);
                }
                serving = false;
                if (wait_timeout <= std::chrono::steady_clock::duration::zero()) {
                    self->removeWaiter(key, node);
                    co_return ClientHandle{ make_error_code(Error::AcquireTimeout) };
                }
                if (!node) {
                    auto& waiters{ self->m_waiters[key] };
                    std::erase_if(waiters, [](auto const& waiter) { return waiter.expired(); });
                    node = std::make_shared<Waiter>(self->m_strand);
                    waiters.push_back(node);
                }
                auto const remaining{ deadline - std::chrono::steady_clock::now() };
                if (remaining <= std::chrono::steady_clock::duration::zero()) {
                    continue;
                }
                auto const waited{ co_await node->event.WaitFor(remaining) };
                if (waited == util::AsyncEvent::WaitResult::Cancelled || waited == util::AsyncEvent::WaitResult::Closed) {
                    self->removeWaiter(key, node);
                    co_return ClientHandle{ canceled };
                }
                serving = waited == util::AsyncEvent::WaitResult::Notified;
            }
        }

        /**
         * @brief Lend an idle client, create one within capacity, or ask the caller to wait.
         * @param key Origin key.
         * @param host Host for a new client.
         * @param port Port for a new client.
         * @param scheme Transport for a new client.
         * @param serving Whether the caller was woken and may bypass earlier waiters.
         * @return Step.
         */
        auto acquireOrCreate(
            std::string const& key,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme,
            bool serving
        ) -> net::awaitable<AcquireStep> {
            // A new caller must not overtake queued waiters.
            if (!serving && hasLiveWaiter(key)) {
                co_return AcquireStep{ ClientHandle{}, true };
            }
            for (;;) {
                auto it{ m_routes.find(key) };
                if (it == m_routes.end() || it->second.idle.empty()) {
                    break;
                }
                auto client{ std::move(it->second.idle.front().client) };
                it->second.idle.pop_front();
                if (m_params.validate_on_borrow) {
                    // While probed, the connection is neither idle nor lent but still counts.
                    ++it->second.validating;
                    bool const alive{ co_await client->AsyncIsAlive() };
                    if (!m_running.load()) {
                        co_return AcquireStep{ ClientHandle{}, true };
                    }
                    it = m_routes.find(key);
                    if (it == m_routes.end()) {
                        co_return AcquireStep{ ClientHandle{}, true };
                    }
                    if (it->second.validating > 0) {
                        --it->second.validating;
                    }
                    if (!alive) {
                        trackDestroyed();
                        m_logger.GetLogger()->debug("client pool: discarding dead idle connection for {}", key);
                        wakeOneWaiter(key);
                        continue;
                    }
                }
                incActive(it->second);
                co_return AcquireStep{ makeHandle(std::move(client)), false };
            }
            if (hasCapacity(key)) {
                co_return AcquireStep{ createClient(key, host, port, scheme), false };
            }
            co_return AcquireStep{ ClientHandle{}, true };
        }

        /**
         * @brief Pool a returned client, or drop it when busy or over capacity.
         * @param client Returned client.
         */
        auto releaseOnStrand(std::unique_ptr<HttpClient> client) -> void {
            if (!m_running.load()) {
                return;
            }
            auto const key{ url::make_url_value(client->Host(), client->Port(), client->GetScheme()) };
            auto const it{ m_routes.find(key) };
            if (it == m_routes.end()) {
                return;
            }
            decActive(it->second);
            if (!client->HasActiveSession() && it->second.active < m_params.max_size) {
                // Reset borrower changes so they cannot leak into the next borrow.
                applySettings(*client);
                it->second.idle.push_back(PooledClient{ std::move(client), std::chrono::steady_clock::now() });
                m_logger.GetLogger()->trace("client pool: returned connection to idle for {}", key);
            } else {
                trackDestroyed();
                m_logger.GetLogger()->trace("client pool: closed returned connection for {}", key);
            }
            wakeOneWaiter(key);
        }

        /**
         * @brief Close every idle connection and wake every waiter; runs on the strand.
         */
        auto teardown() -> void {
            std::vector<WaiterList> pending;
            for (auto& [key, waiters] : m_waiters) {
                pending.push_back(std::move(waiters));
            }
            m_waiters.clear();
            std::vector<std::unique_ptr<HttpClient>> to_close;
            for (auto& [key, route] : m_routes) {
                for (auto& pooled : route.idle) {
                    to_close.push_back(std::move(pooled.client));
                }
            }
            m_routes.clear();
            m_total = 0;
            m_active = 0;
            for (auto& client : to_close) {
                static_cast<void>(client->Close());
            }
            for (auto& waiters : pending) {
                for (auto const& weak : waiters) {
                    if (auto const waiter{ weak.lock() }) {
                        waiter->event.Close();
                    }
                }
            }
        }

        /**
         * @brief Evict connections idle for longer than idle_timeout; runs on the strand.
         */
        auto evictIdle() -> void {
            auto const now{ std::chrono::steady_clock::now() };
            std::vector<std::unique_ptr<HttpClient>> to_close;
            cleanupWaiters();
            for (auto it{ m_routes.begin() }; it != m_routes.end();) {
                auto& route{ it->second };
                for (auto idle{ route.idle.begin() }; idle != route.idle.end();) {
                    if (m_params.idle_timeout > std::chrono::steady_clock::duration::zero() && now - idle->idle_since > m_params.idle_timeout) {
                        to_close.push_back(std::move(idle->client));
                        idle = route.idle.erase(idle);
                        trackDestroyed();
                        wakeOneWaiter(it->first);
                    } else {
                        ++idle;
                    }
                }
                if (route.idle.empty() && route.active == 0 && route.validating == 0) {
                    it = m_routes.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto& client : to_close) {
                static_cast<void>(client->Close());
            }
        }

        auto applySettings(HttpClient& client) const -> void {
            client.SetTimeoutPolicy(m_params.timeout_policy);
            client.SetTimeout(m_params.timeout);
            client.SetMaxRedirects(m_params.max_redirects);
            client.SetTlsContext(m_params.tls_context);
            client.SetDownloadRateLimit(0);
            client.SetUploadRateLimit(0);
            client.SetLogger(m_logger.GetLogger());
        }

        /**
         * @brief Create a client and lend it; the caller checked capacity.
         * @return Handle.
         */
        auto createClient(
            std::string const& key,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme
        ) -> ClientHandle {
            auto& route{ m_routes[key] };
            incActive(route);
            ++m_total;
            // The pool's executor, not its strand: each client serializes on a strand of its own.
            auto client{ std::make_unique<HttpClient>(m_executor, host, port, scheme) };
            applySettings(*client);
            m_logger.GetLogger()->debug("client pool: created connection for {} (total={})", key, m_total.load());
            return makeHandle(std::move(client));
        }

        auto makeHandle(std::unique_ptr<HttpClient> client) -> ClientHandle {
            return ClientHandle{ weak_from_this(), std::move(client) };
        }

        [[nodiscard]] auto hasCapacity(std::string const& key) const -> bool {
            bool route_ok{ m_params.max_size > 0 };
            if (auto const it{ m_routes.find(key) }; it != m_routes.end()) {
                route_ok = it->second.active + it->second.idle.size() + it->second.validating < m_params.max_size;
            }
            return route_ok && (m_params.max_total == 0 || m_total.load() < m_params.max_total);
        }

        auto incActive(RouteState& route) -> void {
            ++route.active;
            ++m_active;
        }

        auto decActive(RouteState& route) -> void {
            if (route.active > 0 && m_active.load() > 0) {
                --route.active;
                --m_active;
            }
        }

        auto trackDestroyed() -> void {
            if (m_total.load() > 0) {
                --m_total;
            }
        }

        [[nodiscard]] auto hasLiveWaiter(std::string const& key) -> bool {
            auto const it{ m_waiters.find(key) };
            if (it == m_waiters.end()) {
                return false;
            }
            std::erase_if(it->second, [](auto const& waiter) { return waiter.expired(); });
            if (it->second.empty()) {
                m_waiters.erase(it);
                return false;
            }
            return true;
        }

        [[nodiscard]] auto canServe(std::string const& key) const -> bool {
            if (auto const it{ m_routes.find(key) }; it != m_routes.end() && !it->second.idle.empty()) {
                return true;
            }
            return hasCapacity(key);
        }

        /**
         * @brief Wake the oldest live waiter; it stays queued until it borrows or gives up.
         * @param key Origin key.
         */
        auto wakeOneWaiter(std::string const& key) -> void {
            auto const it{ m_waiters.find(key) };
            if (it == m_waiters.end()) {
                return;
            }
            auto& waiters{ it->second };
            while (!waiters.empty()) {
                if (auto const waiter{ waiters.front().lock() }) {
                    waiter->event.NotifyOne();
                    return;
                }
                waiters.pop_front();
            }
            m_waiters.erase(it);
        }

        /**
         * @brief Dequeue a waiter and pass a wakeup it no longer needs to the next one.
         * @param key Origin key.
         * @param node Waiter to remove; null does nothing.
         */
        auto removeWaiter(std::string const& key, std::shared_ptr<Waiter> const& node) -> void {
            if (!node) {
                return;
            }
            auto const it{ m_waiters.find(key) };
            if (it == m_waiters.end()) {
                return;
            }
            std::erase_if(it->second, [&](auto const& weak) {
                auto const waiter{ weak.lock() };
                return !waiter || waiter == node;
            });
            if (it->second.empty()) {
                m_waiters.erase(it);
            }
            if (hasLiveWaiter(key) && canServe(key)) {
                wakeOneWaiter(key);
            }
        }

        auto cleanupWaiters() -> void {
            for (auto it{ m_waiters.begin() }; it != m_waiters.end();) {
                std::erase_if(it->second, [](auto const& waiter) { return waiter.expired(); });
                if (it->second.empty()) {
                    it = m_waiters.erase(it);
                } else {
                    ++it;
                }
            }
        }
    };

    // ---- ClientHandle ----

    auto HttpClientPool::ClientHandle::Release() -> void {
        auto const pool{ m_pool.lock() };
        if (pool && m_client) {
            pool->Release(std::move(m_client));
            if (!m_error) {
                m_error = make_error_code(Error::InvalidHandle);
            }
        }
    }

    // ---- HttpClientPool ----

    HttpClientPool::HttpClientPool(
        net::any_io_executor const& executor,
        PoolParams params
    ) : m_impl{ std::make_shared<Impl>(executor, std::move(params)) } {
        m_impl->Start();
    }

    HttpClientPool::~HttpClientPool() {
        if (m_impl) {
            m_impl->Stop();
        }
    }

    auto HttpClientPool::AsyncAcquire(
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme,
        std::chrono::steady_clock::duration wait_timeout
    ) -> net::awaitable<ClientHandle> {
        co_return co_await m_impl->AsyncAcquire(std::string{ host }, port, scheme, wait_timeout);
    }

    auto HttpClientPool::AsyncAcquire(
        std::string_view base_url,
        std::chrono::steady_clock::duration wait_timeout
    ) -> net::awaitable<ClientHandle> {
        auto const parsed{ url::parse_url(base_url) };
        if (!parsed || !(detail::iequals(parsed->scheme, "http") || detail::iequals(parsed->scheme, "https"))) {
            co_return ClientHandle{ make_error_code(Error::InvalidUrl) };
        }
        co_return co_await m_impl->AsyncAcquire(parsed->host, parsed->EffectivePort(), parsed->Transport(), wait_timeout);
    }

    auto HttpClientPool::Logger() const -> std::shared_ptr<spdlog::logger> {
        return m_impl->Logger().GetLogger();
    }

    auto HttpClientPool::SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
        m_impl->SetLogger(std::move(logger));
    }

    auto HttpClientPool::ActiveCount() const -> std::size_t {
        return m_impl->ActiveCount();
    }

    auto HttpClientPool::IdleCount() const -> std::size_t {
        return m_impl->TotalCount() - m_impl->ActiveCount();
    }

    auto HttpClientPool::TotalCount() const -> std::size_t {
        return m_impl->TotalCount();
    }

    auto HttpClientPool::Stop() -> void {
        m_impl->Stop();
    }

    auto HttpClientPool::Stats(
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) const -> std::future<PoolStats> {
        auto const impl{ m_impl };
        return net::co_spawn(
            impl->Strand(),
            [impl, key = url::make_url_value(host, port, scheme)]() mutable -> net::awaitable<PoolStats> {
                co_return co_await impl->AsyncStats(std::move(key));
            },
            net::use_future
        );
    }

    auto HttpClientPool::AsyncStats(
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) const -> net::awaitable<PoolStats> {
        co_return co_await m_impl->AsyncStats(url::make_url_value(host, port, scheme));
    }

} // namespace mhttp::client
