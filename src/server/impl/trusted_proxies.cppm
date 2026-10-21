/**
 * @file trusted_proxies.cppm
 * @brief Parse immutable numeric proxy networks and compare address prefixes without DNS.
 */
module mhttp:server.impl.trusted_proxies;

import std;
import asio;
import :core.error;
import :core.url;
import :utils.string;

namespace mhttp::impl {

    /**
     * @brief Parse only bare, unscoped IPv4/IPv6 literals using strict portable lexical rules.
     * @param text Borrowed numeric address without brackets, port, whitespace, or zone identifier.
     * @return Numeric address, or nullopt without performing name resolution.
     */
    [[nodiscard]] auto parse_proxy_address(std::string_view text) -> std::optional<asio::ip::address> {
        if (text.size() > 45 || (!detail::is_url_ipv4(text) && !detail::is_url_ipv6(text))) {
            return std::nullopt;
        }
        asio::error_code error;
        auto address = asio::ip::make_address(std::string{ text }, error);
        if (error) { return std::nullopt; }
        return address;
    }

    /**
     * @brief Store one address family, prefix length, and network bytes, allowing host bits in CIDRs.
     */
    struct ProxyNetwork {
        std::array<unsigned char, 16> bytes{}; ///< IPv4 occupies the first four bytes.
        unsigned prefix{}; ///< Number of significant high bits, including zero.
        bool ipv6{}; ///< Keep IPv4-mapped IPv6 distinct from IPv4 networks.
    };

    /**
     * @brief Convert a numeric address to family-tagged bytes without retaining socket state.
     * @param address Parsed numeric address.
     * @return Address bytes and the maximum prefix length of its family.
     */
    [[nodiscard]] auto proxy_network(asio::ip::address const& address) -> ProxyNetwork {
        ProxyNetwork result{ .prefix = address.is_v6() ? 128u : 32u, .ipv6 = address.is_v6() };
        if (result.ipv6) {
            std::ranges::copy(address.to_v6().to_bytes(), result.bytes.begin());
        } else { std::ranges::copy(address.to_v4().to_bytes(), result.bytes.begin()); }
        return result;
    }

    /**
     * @brief Own a validated proxy allowlist shared by immutable public policy snapshots.
     * @note Instances are published as const and contain no mutable caches or network resources.
     */
    struct TrustedProxiesImpl {
        std::vector<ProxyNetwork> networks; ///< Owned validated address/prefix pairs.

        /**
         * @brief Validate every source before publishing a policy; an empty list trusts no address.
         * @param sources Bare numeric IPs or CIDRs; values are borrowed only during construction.
         * @return Owned immutable policy or InvalidRequest for malformed addresses or prefixes.
         */
        [[nodiscard]] static auto Create(
            std::vector<std::string> const& sources
        ) -> Result<std::shared_ptr<TrustedProxiesImpl const>> {
            auto result = std::make_shared<TrustedProxiesImpl>();
            result->networks.reserve(sources.size());
            for (auto const& source : sources) {
                auto const separator = source.find('/');
                auto address = parse_proxy_address(std::string_view{ source }.substr(0, separator));
                if (!address) { return std::unexpected{ Error::InvalidRequest }; }
                auto network = proxy_network(*address);
                if (separator != std::string::npos) {
                    auto prefix = utils::parse_integer<unsigned>(
                        std::string_view{ source }.substr(separator + 1));
                    if (!prefix || *prefix > network.prefix) {
                        return std::unexpected{ Error::InvalidRequest };
                    }
                    network.prefix = *prefix;
                }
                result->networks.push_back(network);
            }
            return result;
        }

        /**
         * @brief Compare a parsed address with each configured network of the same address family.
         * @param address Parsed address; scoped/malformed input has already been rejected.
         * @return Whether any prefix matches, with host bits masked on both sides.
         */
        [[nodiscard]] auto Contains(asio::ip::address const& address) const -> bool {
            auto const candidate = proxy_network(address);
            for (auto const& network : networks) {
                if (network.ipv6 != candidate.ipv6) { continue; }
                unsigned remaining = network.prefix;
                bool matches = true;
                for (std::size_t index{}; remaining != 0; ++index) {
                    auto const bits = std::min(remaining, 8u);
                    auto const mask = static_cast<unsigned char>(0xffu << (8 - bits));
                    if ((network.bytes[index] & mask) != (candidate.bytes[index] & mask)) {
                        matches = false;
                        break;
                    }
                    remaining -= bits;
                }
                if (matches) { return true; }
            }
            return false;
        }
    };

} // namespace mhttp::impl
