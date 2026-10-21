/**
 * @file pool_impl.cppm
 * @brief Coordinate bounded origin slots, fair asynchronous acquisition, idle eviction, and terminal shutdown.
 */
module mhttp:client.impl.pool;

import std;
import asio;
import :core.impl.io_error;
import :core.impl.bridge;
import :client.impl.pool_lease;
import :client.pool_options;
import :client.request_policy;

namespace mhttp::impl {

    /**
     * @brief Own a thread-safe capacity ledger with asynchronous waits confined to a pool strand.
     * @note The mutex protects only metadata; no network operation or blocking wait runs under it.
     *       Loan returns may originate on any Task's scheduler or a caller destroying an unstarted Task.
     */
    class ClientPoolImpl : public ClientPoolReturner, public std::enable_shared_from_this<ClientPoolImpl> {
    private:
        using Clock = std::chrono::steady_clock; ///< Monotonic acquisition and idle clock.
        using Loan = std::shared_ptr<ClientPoolLease>; ///< Exclusive slot shared only by its handle and operations.
        /**
         * @brief Own a returned client and its idle timestamp without retaining its old loan.
         */
        struct Idle {
            std::unique_ptr<client::HttpClient> client; ///< Sole returned public client owner.
            Clock::time_point since; ///< Most recent loan return time.
        };
        /**
         * @brief Count one origin's outstanding and idle client slots.
         */
        struct Route {
            std::size_t active{}; ///< Loans not yet returned by their final owner.
            std::list<Idle> idle; ///< Returned clients in oldest-first order.
        };
        /**
         * @brief Retain one queue entry and its timer until every notification and cancellation drains.
         */
        struct Waiter {
            Url origin; ///< Validated destination, independent of caller strings.
            std::string key; ///< Canonical scheme/host/port identity.
            asio::steady_timer timer; ///< Accessed only on the pool strand.
            Clock::time_point deadline; ///< Absolute capacity wait deadline.
            Loan loan; ///< Granted capacity, reserved before the waiter is notified.
            std::optional<Error> error; ///< Timeout, shutdown, or creation failure.
            /**
             * @brief Own validated demand before publishing it to the queue.
             * @param url Owned destination.
             * @param name Owned canonical origin key.
             * @param executor Timer strand.
             * @param until Absolute wait deadline, including zero-wait fast acquisition.
             */
            Waiter(
                Url url,
                std::string name,
                asio::any_io_executor executor,
                Clock::time_point until
            )
                : origin{ std::move(url) }, key{ std::move(name) }, timer{ executor }, deadline{ until } {}
        };
        using Waiters = std::list<std::shared_ptr<Waiter>>; ///< Nodes splice between queued and ready without allocation.
        using Active = std::unordered_map<ClientPoolLease*, std::weak_ptr<ClientPoolLease>>; ///< Weak loan registry.

        Runtime& m_runtime_owner; ///< Borrowed public runtime, longer-lived than pool, loans, and issued Tasks.
        std::shared_ptr<RuntimeImpl> m_runtime; ///< Execution state retained through asynchronous cleanup.
        asio::any_io_executor m_executor; ///< Strand for wait timers and periodic maintenance.
        std::shared_ptr<asio::steady_timer> m_timer; ///< Retained independently by a posted shutdown cancellation.
        client::ClientPoolOptions m_options; ///< Immutable validated configuration.
        std::optional<Error> m_initialization_error; ///< Invalid configuration before any timer/network activity.
        mutable std::mutex m_mutex; ///< Serialize capacity, queues, and statistics from all threads.
        std::unordered_map<std::string, Route> m_routes; ///< Only origins owning slots remain in this map.
        Active m_active; ///< Weak loans used to cancel borrowed clients at terminal stop.
        Waiters m_waiters; ///< FIFO admission order; unrelated blocked origins do not prevent eligible grants.
        std::size_t m_active_count{}; ///< Consistent count of all outstanding loans.
        std::size_t m_idle_count{}; ///< Consistent count of all returned slots.
        bool m_stopped{}; ///< Terminal after explicit stop or the first attached runtime generation stops.
        bool m_started{}; ///< Maintenance has been armed on the strand.
        std::optional<std::stop_callback<std::function<void()>>> m_runtime_stop; ///< Weak generation shutdown link.

        /**
         * @brief Compute a consistent scheme/host/port key, canonicalizing numeric address spelling.
         * @param origin Validated HTTP URL; path/query/fragment do not affect pool identity.
         * @return Owned canonical key.
         */
        static auto key(Url const& origin) -> std::string {
            auto host = std::string{ origin.Host() };
            asio::error_code error;
            auto address = asio::ip::make_address(host, error);
            if (!error) { host = address.to_string(); }
            return std::format("http://{}:{}", host.contains(':') ? '[' + host + ']' : host, origin.Port());
        }

        /**
         * @brief Test immediate capacity while holding the ledger mutex.
         * @param name Canonical origin key.
         * @return Whether an idle slot or a new bounded slot can serve this origin.
         */
        auto available(std::string const& name) const -> bool {
            auto const route = m_routes.find(name);
            if (route != m_routes.end() && !route->second.idle.empty()) { return true; }
            auto const count = route == m_routes.end() ? 0 : route->second.active;
            return count < m_options.max_per_origin && m_active_count + m_idle_count < m_options.max_total;
        }

        /**
         * @brief Reserve one available slot, publishing its weak return link only after throwing allocations finish.
         * @param origin Validated destination used for a new lazy client.
         * @param name Canonical immutable origin key.
         * @return Owned exclusive loan; callers hold the ledger mutex and have checked available.
         */
        auto borrow(Url const& origin, std::string const& name) -> Loan {
            auto [it, inserted] = m_routes.try_emplace(name);
            auto& route = it->second;
            try {
                auto loan = std::make_shared<ClientPoolLease>();
                loan->key = name;
                auto client = route.idle.empty() ? std::make_unique<client::HttpClient>(m_runtime_owner,
                    origin.Host(), origin.Port(), m_options.client_options) : nullptr;
                m_active.emplace(loan.get(), loan);
                if (client) { loan->client = std::move(client); }
                else {
                    loan->client = std::move(route.idle.front().client);
                    route.idle.pop_front();
                    --m_idle_count;
                }
                ++route.active;
                ++m_active_count;
                loan->pool = shared_from_this();
                loan->client->m_pool_lease = loan;
                return loan;
            } catch (...) {
                if (inserted) { m_routes.erase(it); }
                throw;
            }
        }

        /**
         * @brief Reserve slots for queued eligible waiters in arrival order without global head-of-line blocking.
         * @param ready Nodes to notify outside the mutex.
         */
        auto service(Waiters& ready) -> void {
            auto const now = Clock::now();
            for (auto it = m_waiters.begin(); it != m_waiters.end();) {
                auto& waiter = **it;
                if (waiter.deadline <= now) { waiter.error = Error::AcquireTimeout; }
                else if (available(waiter.key)) {
                    try { waiter.loan = borrow(waiter.origin, waiter.key); }
                    catch (...) { waiter.error = Error::Unknown; }
                }
                if (waiter.loan || waiter.error) { auto current = it++; ready.splice(ready.end(), m_waiters, current); }
                else { ++it; }
            }
        }

        /**
         * @brief Publish granted/failed outcomes without touching timers from a foreign return thread.
         * @param ready Owned completed queue nodes, retained by their awaiting coroutine frames.
         */
        static auto notify(Waiters ready) noexcept -> void {
            for (auto const& waiter : ready) {
                try {
                    asio::post(waiter->timer.get_executor(), [weak = std::weak_ptr{ waiter }] {
                        if (auto owner = weak.lock()) { owner->timer.expires_at(Clock::time_point::min()); }
                    });
                } catch (...) {}
            }
        }

        /**
         * @brief Publish reserved outcomes even if admission throws before its normal notification point.
         */
        struct Notifications {
            Waiters ready; ///< Completed nodes awaiting strand notifications.
            /**
             * @brief Transfer every outstanding notification outside the metadata lock.
             */
            ~Notifications() { notify(std::move(ready)); }
        };

        /**
         * @brief Remove a queued or granted demand on every exit, releasing its loan outside the mutex.
         */
        struct Registration {
            ClientPoolImpl& pool; ///< Owned by the enclosing acquisition frame.
            std::shared_ptr<Waiter> waiter; ///< Demand retained through timer completion.
            /**
             * @brief Retire capacity safely after normal completion, cancellation, or an exception.
             */
            ~Registration() {
                Loan loan;
                {
                    std::lock_guard lock{ pool.m_mutex };
                    pool.m_waiters.remove(waiter);
                    loan = std::move(waiter->loan);
                }
            }
        };

        /**
         * @brief Arm the next independent idle check, retaining only a weak pool reference in its callback.
         * @note Called only on the pool strand; runtime/explicit Stop retires the pending wait.
         */
        auto scheduleMaintenance() -> void {
            {
                std::lock_guard lock{ m_mutex };
                if (m_stopped || m_options.idle_timeout.count() == 0) { return; }
                m_started = true;
            }
            m_timer->expires_after(m_options.idle_check_interval);
            m_timer->async_wait([weak = weak_from_this()](asio::error_code error) {
                if (auto owner = weak.lock(); owner && !error) { owner->maintain(); owner->scheduleMaintenance(); }
            });
        }

        /**
         * @brief Evict expired idle slots and grant newly available capacity without destroying clients under the mutex.
         */
        auto maintain() -> void {
            std::list<Idle> retired;
            Waiters ready;
            {
                std::lock_guard lock{ m_mutex };
                if (m_stopped) { return; }
                auto const now = Clock::now();
                for (auto it = m_routes.begin(); it != m_routes.end();) {
                    auto& idle = it->second.idle;
                    while (!idle.empty() && now - idle.front().since >= m_options.idle_timeout) {
                        retired.splice(retired.end(), idle, idle.begin());
                        --m_idle_count;
                    }
                    if (it->second.active == 0 && idle.empty()) { it = m_routes.erase(it); }
                    else { ++it; }
                }
                service(ready);
            }
            notify(std::move(ready));
        }

        /**
         * @brief Acquire on the pool strand while all caller strings, queue nodes, and timers remain owned.
         * @param self Owned pool through cancellation and cleanup.
         * @param origin Validated destination, or a prior syntax/scheme error.
         * @param wait Relative capacity wait; nonpositive values try once without queuing.
         * @return Reserved loan or bounded capacity/configuration/cancellation failure.
         */
        static auto acquire(
            std::shared_ptr<ClientPoolImpl> self,
            Result<Url> origin,
            Clock::duration wait
        ) -> asio::awaitable<Result<Loan>> {
            co_await asio::this_coro::throw_if_cancelled(false);
            if (self->m_initialization_error) { co_return std::unexpected{ *self->m_initialization_error }; }
            if (!origin) { co_return std::unexpected{ origin.error() }; }
            if (origin->Scheme() != "http") { co_return std::unexpected{ Error::UnsupportedScheme }; }
            auto const parent = co_await asio::this_coro::cancellation_state;
            if (parent.cancelled() != asio::cancellation_type::none || self->Stopped()) { co_return std::unexpected{ Error::Canceled }; }
            auto token = self->m_runtime->StopToken();
            if (!token) { co_return std::unexpected{ token.error() }; }
            if (!self->m_runtime_stop) {
                self->m_runtime_stop.emplace(*token, [weak = self->weak_from_this()] {
                    if (auto owner = weak.lock()) { owner->Stop(); }
                });
                self->scheduleMaintenance();
            }
            if (wait > Clock::duration::max() / 2) { co_return std::unexpected{ Error::InvalidRequest }; }
            auto const now = Clock::now();
            auto const until = now + std::max(wait, Clock::duration::zero());
            auto name = key(*origin);
            auto waiter = std::make_shared<Waiter>(std::move(*origin), std::move(name), self->m_executor, until);
            waiter->timer.expires_at(until);
            Registration registration{ *self, waiter };
            Notifications notifications;
            bool queued{};
            {
                std::lock_guard lock{ self->m_mutex };
                self->service(notifications.ready);
                if (self->m_stopped) { waiter->error = Error::Canceled; }
                else if (self->available(waiter->key)) {
                    try { waiter->loan = self->borrow(waiter->origin, waiter->key); }
                    catch (...) { waiter->error = Error::Unknown; }
                }
                else if (wait <= Clock::duration::zero()) { waiter->error = Error::AcquireTimeout; }
                else if (self->m_waiters.size() >= self->m_options.max_waiters) { waiter->error = Error::Busy; }
                else { self->m_waiters.push_back(waiter); queued = true; }
            }
            notify(std::move(notifications.ready));
            if (queued) {
                static_cast<void>(co_await waiter->timer.async_wait(asio::as_tuple(asio::use_awaitable)));
            }
            Loan loan;
            std::optional<Error> error;
            {
                std::lock_guard lock{ self->m_mutex };
                self->m_waiters.remove(waiter);
                loan = std::move(waiter->loan);
                error = waiter->error;
                if (self->m_stopped || parent.cancelled() != asio::cancellation_type::none) { error = Error::Canceled; }
                else if (!loan && !error) { error = Error::AcquireTimeout; }
            }
            if (error) { co_return std::unexpected{ *error }; }
            co_return std::move(loan);
        }

        /**
         * @brief Normalize bridge cancellation while retaining the pool before initial suspension.
         * @param self Owned pool state.
         * @param origin Owned destination or syntax failure.
         * @param wait Capacity deadline.
         * @return Loan or recoverable admission failure.
         */
        static auto acquireTask(
            std::shared_ptr<ClientPoolImpl> self,
            Result<Url> origin,
            Clock::duration wait
        ) -> Task<Result<Loan>> {
            try {
                auto result = co_await ToTask(self->m_executor, acquire(self, std::move(origin), wait));
                if (!result) { co_return std::unexpected{ result.error() }; }
                if (self->m_options.validate_on_borrow && (*result)->client->IsOpen()) {
                    auto alive = co_await (*result)->client->IsAlive();
                    if (!alive) { co_return std::unexpected{ alive.error() }; }
                }
                if (self->Stopped()) { co_return std::unexpected{ Error::Canceled }; }
                co_return std::move(*result);
            }
            catch (std::system_error const& error) {
                if (error.code() == Error::Canceled) { co_return std::unexpected{ Error::Canceled }; }
                throw;
            }
        }

    public:
        /**
         * @brief Own bounded pool state without connecting or starting a background operation.
         * @param runtime Public execution owner, borrowed through all pool and loan operations.
         * @param options Owned immutable capacities and client configuration.
         */
        ClientPoolImpl(Runtime& runtime, client::ClientPoolOptions options)
            : m_runtime_owner{ runtime }, m_runtime{ runtime.m_impl },
                m_executor{ asio::make_strand(m_runtime->Context()) },
                m_timer{ std::make_shared<asio::steady_timer>(m_executor) }, m_options{ std::move(options) } {
            auto const max = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max() / 2);
            auto valid = detail::validate_client_options(m_options.client_options);
            if (!valid) { m_initialization_error = valid.error(); }
            else if (m_options.max_per_origin == 0 || m_options.max_total == 0 || m_options.max_waiters == 0
                || m_options.idle_timeout.count() < 0 || m_options.idle_timeout > max
                || m_options.idle_check_interval.count() <= 0 || m_options.idle_check_interval > max) {
                m_initialization_error = Error::InvalidRequest;
            }
        }

        /**
         * @brief Inspect constructor validation without starting work.
         * @return Invalid configuration error, or no value.
         */
        [[nodiscard]] auto InitializationError() const noexcept -> std::optional<Error> { return m_initialization_error; }
        /**
         * @brief Inspect terminal shutdown from any thread.
         * @return Whether future acquisitions must be rejected.
         */
        [[nodiscard]] auto Stopped() const -> bool { std::lock_guard lock{ m_mutex }; return m_stopped; }
        /**
         * @brief Issue an owned lazy acquisition retaining its validated origin before initial suspension.
         * @param origin Owned parsed URL or an earlier validation error.
         * @param wait Relative capacity deadline.
         * @return Lazy loan operation, resuming on the caller's inherited Task scheduler.
         */
        auto Acquire(Result<Url> origin, Clock::duration wait) -> Task<Result<Loan>> {
            return acquireTask(shared_from_this(), std::move(origin), wait);
        }
        /**
         * @brief Snapshot either the complete pool or one normalized origin under a short metadata lock.
         * @param origin Optional validated filter; absent selects the entire pool.
         * @return Consistent active, idle, and waiting counts.
         */
        [[nodiscard]] auto Stats(
            std::optional<Url> const& origin = std::nullopt
        ) const -> client::ClientPoolStats {
            auto const name = origin ? key(*origin) : std::string{};
            std::lock_guard lock{ m_mutex };
            if (!origin) { return { m_active_count, m_idle_count, m_waiters.size() }; }
            auto const route = m_routes.find(name);
            auto const waiting = static_cast<std::size_t>(std::ranges::count_if(m_waiters,
                [&name](auto const& waiter) { return waiter->key == name; }));
            if (route == m_routes.end()) { return { .waiting = waiting }; }
            return { route->second.active, route->second.idle.size(), waiting };
        }
        /**
         * @brief Return capacity from the last loan reference without blocking or canceling live request Tasks.
         * @param lease Retiring identity, present in the weak registry unless Stop already removed it.
         * @param name Owned canonical origin key.
         * @param client Sole returned wrapper; destruction occurs outside the metadata mutex.
         */
        auto Return(
            ClientPoolLease* lease,
            std::string name,
            std::unique_ptr<client::HttpClient> client
        ) noexcept -> void override {
            Waiters ready;
            try {
                {
                    std::lock_guard lock{ m_mutex };
                    auto it = m_routes.find(name);
                    if (it == m_routes.end()) { return; }
                    m_active.erase(lease);
                    --m_active_count;
                    --it->second.active;
                    if (!m_stopped) {
                        try { it->second.idle.push_back({ std::move(client), Clock::now() }); ++m_idle_count; }
                        catch (...) {}
                    }
                    if (it->second.active == 0 && it->second.idle.empty()) { m_routes.erase(it); }
                    service(ready);
                }
                notify(std::move(ready));
            } catch (...) { Stop(); }
        }
        /**
         * @brief Stop terminally from any thread, cancel borrowed clients, and wake all queued acquisitions.
         * @note This is nonblocking. Outstanding handles/Tasks continue owning their slots until retirement.
         */
        auto Stop() noexcept -> void {
            std::list<Idle> retired;
            Waiters ready;
            Active active;
            bool cancel_timer{};
            {
                std::lock_guard lock{ m_mutex };
                if (m_stopped) { return; }
                m_stopped = true;
                cancel_timer = m_started;
                active.swap(m_active);
                ready.splice(ready.end(), m_waiters);
                for (auto const& waiter : ready) { waiter->error = Error::Canceled; }
                for (auto it = m_routes.begin(); it != m_routes.end();) {
                    retired.splice(retired.end(), it->second.idle);
                    if (it->second.active == 0) { it = m_routes.erase(it); }
                    else { ++it; }
                }
                m_idle_count = 0;
            }
            for (auto const& [identity, weak] : active) {
                if (auto loan = weak.lock()) { try { loan->client->m_impl->StopPermanently(); } catch (...) {} }
            }
            notify(std::move(ready));
            if (cancel_timer) {
                try { asio::post(m_executor, [timer = m_timer] { timer->cancel(); }); } catch (...) {}
            }
        }
    };

} // namespace mhttp::impl
