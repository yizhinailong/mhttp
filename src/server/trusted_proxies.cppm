/**
 * @file trusted_proxies.cppm
 * @brief Match peer addresses against the configured trusted proxy networks.
 */
module;

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:trusted_proxies;

import std;
import :config;

namespace mhttp::detail {

    /**
     * @brief Immutable set of trusted proxy networks (CIDRs or single addresses).
     */
    class TrustedProxies {
    private:
        struct Cidr {
            net::ip::address network;   ///< Network address.
            unsigned short prefix_length; ///< Significant leading bits.
        };

        std::vector<Cidr> m_cidrs; ///< Trusted networks.

    public:
        /**
         * @brief Parse CIDRs ("10.0.0.0/8", "fd00::/8") and single addresses (host routes).
         * @param cidrs Entries to parse.
         * @throws std::invalid_argument If an entry is neither a CIDR nor an address.
         */
        explicit TrustedProxies(std::vector<std::string> const& cidrs) {
            m_cidrs.reserve(cidrs.size());
            for (auto const& cidr : cidrs) {
                boost::system::error_code ec;
                if (cidr.find('/') != std::string::npos) {
                    if (auto const v4{ net::ip::make_network_v4(cidr, ec) }; !ec) {
                        m_cidrs.push_back({ v4.address(), v4.prefix_length() });
                        continue;
                    }
                    ec.clear();
                    if (auto const v6{ net::ip::make_network_v6(cidr, ec) }; !ec) {
                        m_cidrs.push_back({ v6.address(), v6.prefix_length() });
                        continue;
                    }
                } else {
                    if (auto const v4{ net::ip::make_address_v4(cidr, ec) }; !ec) {
                        m_cidrs.push_back({ v4, 32 });
                        continue;
                    }
                    ec.clear();
                    if (auto const v6{ net::ip::make_address_v6(cidr, ec) }; !ec) {
                        m_cidrs.push_back({ v6, 128 });
                        continue;
                    }
                }
                throw std::invalid_argument{ "invalid trusted proxy CIDR or address: " + cidr };
            }
        }

        /**
         * @brief Report whether an address lies in a trusted network.
         * @param address Address to test.
         * @return True when some network of the same family contains it.
         */
        [[nodiscard]] auto Contains(net::ip::address const& address) const -> bool {
            for (auto const& cidr : m_cidrs) {
                if (cidr.network.is_v4() != address.is_v4()) {
                    continue;
                }
                bool const matches{ cidr.network.is_v4() ? prefixMatches(cidr.network.to_v4().to_bytes(), address.to_v4().to_bytes(), cidr.prefix_length)
                                                         : prefixMatches(cidr.network.to_v6().to_bytes(), address.to_v6().to_bytes(), cidr.prefix_length) };
                if (matches) {
                    return true;
                }
            }
            return false;
        }

    private:
        /**
         * @brief Compare the leading bits of two addresses of the same family.
         * @param network Network bytes.
         * @param candidate Candidate bytes.
         * @param prefix_length Number of leading bits to compare.
         * @return True when the prefixes are equal.
         */
        static auto prefixMatches(
            auto const& network,
            auto const& candidate,
            unsigned prefix_length
        ) -> bool {
            for (std::size_t i{ 0 }; i < network.size(); ++i) {
                auto const take{ std::min(8U, prefix_length) };
                auto const mask{ static_cast<unsigned char>(take == 0 ? 0U : (0xFFU << (8 - take)) & 0xFFU) };
                if ((candidate[i] & mask) != (network[i] & mask)) {
                    return false;
                }
                prefix_length -= take;
            }
            return true;
        }
    };

} // namespace mhttp::detail
