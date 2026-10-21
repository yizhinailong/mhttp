/**
 * @file pool_lease.cppm
 * @brief Keep a borrowed client and slot alive through handles and every issued lazy transfer Task.
 */
module mhttp:client.impl.pool_lease;

import std;
import :client.http_client;

namespace mhttp::impl {

    struct ClientPoolLease;

    /**
     * @brief Return a final loan without introducing a strong ownership cycle with its pool.
     */
    class ClientPoolReturner {
    public:
        /**
         * @brief Release the implementation after all independently retained operations retire.
         */
        virtual ~ClientPoolReturner() = default;
        /**
         * @brief Return one uniquely owned client from any thread without waiting for network activity.
         * @param lease Identity of the retiring registered loan, valid during this call only.
         * @param key Owned normalized origin key.
         * @param client Sole public client owner, with no outstanding loan-bound request Tasks.
         */
        virtual auto Return(
            ClientPoolLease* lease,
            std::string key,
            std::unique_ptr<client::HttpClient> client
        ) noexcept -> void = 0;
    };

    /**
     * @brief Own one exclusive client until its handle and all issued transfer Tasks have retired.
     * @note The client holds only a weak link back to this lease; the pool also records weak loans.
     */
    struct ClientPoolLease {
        std::unique_ptr<client::HttpClient> client; ///< Exclusive public wrapper retained through request completion.
        std::string key; ///< Normalized immutable origin used when returning capacity.
        std::weak_ptr<ClientPoolReturner> pool; ///< Optional return owner; expired pools simply destroy the client.
        /**
         * @brief Return the last outstanding reference without canceling a request that still retains a loan.
         */
        ~ClientPoolLease() {
            if (auto owner = pool.lock()) { owner->Return(this, std::move(key), std::move(client)); }
        }
    };

} // namespace mhttp::impl
