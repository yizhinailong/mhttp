/**
 * @file client_pool.cppm
 * @brief Expose bounded asynchronous HTTP client borrowing, statistics, and terminal shutdown.
 */
export module mhttp:client.client_pool;

import std;
import :client.impl.pool;
import :client.request_policy;
export import :client.pool_options;
export import :client.client_handle;

export namespace mhttp::client {

    /**
     * @brief Own bounded reusable clients grouped by normalized HTTP scheme, host, and port.
     * @note Runtime must outlive the pool, handles, and issued Tasks. Acquire owns its inputs before
     *       initial suspension. Destruction and Stop cancel current work without blocking; Stop is terminal.
     *       Acquisitions/returns/statistics/Stop are thread-safe; moving or destroying the pool requires
     *       external synchronization. Returning a client preserves its mutable defaults and authentication.
     */
    class ClientPool {
    private:
        std::shared_ptr<impl::ClientPoolImpl> m_impl; ///< State retained by issued acquisition Tasks.

        /**
         * @brief Convert an independently owned acquisition result into its public move-only handle.
         * @param operation Owned implementation operation.
         * @return Handle or recoverable acquisition failure.
         */
        static auto wrap(
            Task<Result<std::shared_ptr<impl::ClientPoolLease>>> operation
        ) -> Task<Result<ClientHandle>> {
            auto loan = co_await std::move(operation);
            if (!loan) { co_return std::unexpected{ loan.error() }; }
            co_return ClientHandle{ std::move(*loan) };
        }
        /**
         * @brief Represent an operation issued on a moved-from pool without borrowing its wrapper.
         * @return Terminal cancellation.
         */
        static auto canceled() -> Task<Result<ClientHandle>> { co_return std::unexpected{ Error::Canceled }; }
        /**
         * @brief Own parsed demand before returning a lazy Task.
         * @param origin Owned URL or validation error.
         * @param wait Relative capacity deadline.
         * @return Owned handle operation.
         */
        auto acquire(
            Result<Url> origin,
            std::chrono::steady_clock::duration wait
        ) -> Task<Result<ClientHandle>> {
            return m_impl ? wrap(m_impl->Acquire(std::move(origin), wait)) : canceled();
        }
        /**
         * @brief Validate a filter consistently with acquisition.
         * @param origin Parsed destination.
         * @return Origin statistics or a syntax/scheme/moved-from error.
         */
        auto stats(Result<Url> origin) const -> Result<ClientPoolStats> {
            if (!m_impl) { return std::unexpected{ Error::Canceled }; }
            if (!origin) { return std::unexpected{ origin.error() }; }
            if (origin->Scheme() != "http") { return std::unexpected{ Error::UnsupportedScheme }; }
            return m_impl->Stats(std::move(*origin));
        }

    public:
        /**
         * @brief Own pool configuration without connecting or starting timers.
         * @param runtime Longer-lived execution owner.
         * @param options Owned immutable capacity, idle, and client defaults.
         */
        explicit ClientPool(Runtime& runtime, ClientPoolOptions options = {}) :
            m_impl{ std::make_shared<impl::ClientPoolImpl>(runtime, std::move(options)) } {}
        /**
         * @brief Transfer pool ownership, leaving the source stopped and empty.
         * @param other Source pool.
         */
        ClientPool(ClientPool&& other) noexcept = default;
        /**
         * @brief Stop the destination's previous pool and transfer ownership.
         * @param other Source pool, left empty unless self-assigned.
         * @return This pool.
         */
        auto operator=(ClientPool&& other) noexcept -> ClientPool& {
            if (this != &other) { Stop(); m_impl = std::move(other.m_impl); }
            return *this;
        }
        /**
         * @brief Prevent duplicate public shutdown owners.
         * @param other Unused source.
         */
        ClientPool(ClientPool const& other) = delete;
        /**
         * @brief Prevent copying public shutdown ownership.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(ClientPool const& other) -> ClientPool& = delete;
        /**
         * @brief Stop terminally without waiting for handles or issued Tasks.
         */
        ~ClientPool() { Stop(); }
        /**
         * @brief Inspect configuration validation without starting any operation.
         * @return Invalid configuration, Canceled for a moved-from pool, or no error.
         */
        [[nodiscard]] auto InitializationError() const noexcept -> std::optional<Error> {
            return m_impl ? m_impl->InitializationError() : std::optional{ Error::Canceled };
        }
        /**
         * @brief Issue a loan for a copied host/port origin without opening a connection.
         * @param host Hostname or IPv6 literal, optionally bracketed.
         * @param port TCP port.
         * @param wait Capacity wait; nonpositive values try once; excessive durations are rejected.
         * @return Handle, AcquireTimeout, Busy for a full wait queue, or validation/lifecycle failure.
         */
        [[nodiscard]] auto Acquire(
            std::string_view host,
            std::uint16_t port,
            std::chrono::steady_clock::duration wait = std::chrono::seconds{ 3 }
        ) -> Task<Result<ClientHandle>> { return acquire(detail::client_origin(host, port), wait); }
        /**
         * @brief Issue a loan using an owned normalized URL origin; path/query/fragment do not affect identity.
         * @param url Absolute HTTP URL; HTTPS reports UnsupportedScheme.
         * @param wait Capacity deadline; zero or negative tries once without queuing.
         * @return Exclusive handle whose client's default request target is the origin root.
         */
        [[nodiscard]] auto Acquire(
            std::string_view url,
            std::chrono::steady_clock::duration wait = std::chrono::seconds{ 3 }
        ) -> Task<Result<ClientHandle>> { return acquire(Url::Parse(url), wait); }
        /**
         * @brief Inspect one consistent aggregate snapshot without scheduling work.
         * @return Active/idle/waiting counts, or zeroes for a moved-from pool.
         */
        [[nodiscard]] auto Stats() const -> ClientPoolStats { return m_impl ? m_impl->Stats() : ClientPoolStats{}; }
        /**
         * @brief Inspect a normalized host/port origin, including queued demand without allocated slots.
         * @param host Valid hostname or address, copied during this call.
         * @param port Origin port.
         * @return Origin snapshot or validation failure.
         */
        [[nodiscard]] auto Stats(
            std::string_view host,
            std::uint16_t port
        ) const -> Result<ClientPoolStats> {
            return stats(detail::client_origin(host, port));
        }
        /**
         * @brief Inspect the same URL-origin identity used by Acquire, ignoring path/query/fragment.
         * @param url Absolute HTTP URL.
         * @return Origin snapshot or validation failure.
         */
        [[nodiscard]] auto Stats(std::string_view url) const -> Result<ClientPoolStats> { return stats(Url::Parse(url)); }
        /**
         * @brief Observe whether explicit shutdown, destruction, or an attached runtime stop is terminal.
         * @return True after shutdown or for a moved-from pool.
         */
        [[nodiscard]] auto Stopped() const -> bool { return !m_impl || m_impl->Stopped(); }
        /**
         * @brief Cancel borrowed clients, discard idle clients, and wake queued acquisitions without blocking.
         * @note Active counts remain until the last handles/issued Tasks retire. Runtime restart does not revive a pool.
         */
        auto Stop() noexcept -> void { if (m_impl) { m_impl->Stop(); } }
    };

} // namespace mhttp::client
