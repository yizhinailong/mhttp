/**
 * @file trusted_proxies.cppm
 * @brief Expose immutable trusted proxy address and CIDR configuration without backend types.
 */
export module mhttp:server.trusted_proxies;

import std;
import :server.impl.trusted_proxies;
export import :core.error;

export namespace mhttp::server {

    /**
     * @brief Share an immutable IPv4/IPv6 proxy allowlist; the default value trusts no address.
     * @note Copies retain their original policy when another copy is replaced. Concurrent reads
     *       of one policy are safe; assignment requires external synchronization.
     */
    class TrustedProxies {
    private:
        std::shared_ptr<impl::TrustedProxiesImpl const> m_impl; ///< Nullable immutable owned allowlist.

        /**
         * @brief Retain a completely validated policy without borrowing caller storage.
         * @param state Owned immutable allowlist.
         */
        explicit TrustedProxies(std::shared_ptr<impl::TrustedProxiesImpl const> state) :
            m_impl{ std::move(state) } {}

    public:
        /**
         * @brief Construct an empty policy that trusts no peer.
         */
        TrustedProxies() = default;

        /**
         * @brief Validate and own numeric addresses or CIDRs without DNS resolution.
         * @param sources Bare IPv4/IPv6 addresses, optionally followed by /0 through /32 or /128.
         * @return Immutable policy or InvalidRequest; any invalid source rejects the whole list.
         * @note Host bits in CIDRs are masked. Zones, brackets, ports, hostnames, whitespace, and
         *       legacy IPv4 spellings are rejected. IPv4-mapped IPv6 matches IPv6 networks only.
         */
        [[nodiscard]] static auto Create(
            std::vector<std::string> const& sources
        ) -> Result<TrustedProxies> {
            auto state = impl::TrustedProxiesImpl::Create(sources);
            if (!state) { return std::unexpected{ state.error() }; }
            return TrustedProxies{ std::move(*state) };
        }

        /**
         * @brief Check a bare numeric address against this immutable allowlist.
         * @param address Borrowed IPv4/IPv6 literal without scope, brackets, port, or whitespace.
         * @return Whether a same-family network matches; invalid input always returns false.
         */
        [[nodiscard]] auto Contains(std::string_view address) const -> bool {
            if (!m_impl) { return false; }
            auto parsed = impl::parse_proxy_address(address);
            return parsed && m_impl->Contains(*parsed);
        }
    };

} // namespace mhttp::server
