/**
 * @file client_handle.cppm
 * @brief Own one exclusive client pool loan with move-only, idempotent release.
 */
export module mhttp:client.client_handle;

import std;
import :client.impl.pool_lease;
export import :client.http_client;

export namespace mhttp::client {

    class ClientPool;

    /**
     * @brief Borrow one single-flight client until this handle and every issued Task release their loan.
     * @note Runtime must outlive all handles and Tasks. Borrowed pointers become invalid after Release,
     *       move assignment, or destruction. Issued lazy/streaming Tasks independently retain the loan.
     *       One handle's access and release require caller synchronization; distinct handles are independent.
     */
    class ClientHandle {
    private:
        std::shared_ptr<impl::ClientPoolLease> m_lease; ///< Exclusive loan shared only with issued operations.
        friend class ClientPool;
        /**
         * @brief Adopt a successfully reserved pool loan.
         * @param lease Owned reserved client.
         */
        explicit ClientHandle(std::shared_ptr<impl::ClientPoolLease> lease) : m_lease{ std::move(lease) } {}

    public:
        /**
         * @brief Construct an empty handle.
         */
        ClientHandle() = default;
        /**
         * @brief Transfer a loan, leaving the source empty.
         * @param other Source handle.
         */
        ClientHandle(ClientHandle&& other) noexcept = default;
        /**
         * @brief Return any previous loan and transfer the source's loan.
         * @param other Source handle, left empty unless self-assigned.
         * @return This handle.
         */
        auto operator=(ClientHandle&& other) noexcept -> ClientHandle& = default;
        /**
         * @brief Prevent duplicate exclusive borrower handles.
         * @param other Unused source.
         */
        ClientHandle(ClientHandle const& other) = delete;
        /**
         * @brief Prevent copying exclusive borrowers.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(ClientHandle const& other) -> ClientHandle& = delete;
        /**
         * @brief Return this handle's reference; issued Tasks may keep the loan active.
         */
        ~ClientHandle() = default;
        /**
         * @brief Test whether this handle owns a loan.
         * @return True for a nonempty handle, including after its pool stops.
         */
        explicit operator bool() const noexcept { return static_cast<bool>(m_lease); }
        /**
         * @brief Borrow the client while the handle remains nonempty.
         * @return Client pointer, or null for an empty handle.
         */
        [[nodiscard]] auto Get() const noexcept -> HttpClient* { return m_lease ? m_lease->client.get() : nullptr; }
        /**
         * @brief Access a nonempty handle's client.
         * @return Borrowed client; the caller must ensure the handle is nonempty.
         */
        auto operator->() const noexcept -> HttpClient* { return Get(); }
        /**
         * @brief Dereference a nonempty handle.
         * @return Borrowed client; the caller must ensure the handle is nonempty.
         */
        auto operator*() const noexcept -> HttpClient& { return *Get(); }
        /**
         * @brief Release this handle idempotently without canceling its already issued Tasks.
         */
        auto Release() noexcept -> void { m_lease.reset(); }
    };

} // namespace mhttp::client
