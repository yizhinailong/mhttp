/**
 * @file socket_options.cppm
 * @brief Apply platform-specific listener reservation, IPv6 mode, and accepted-socket options.
 */
module;

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

module mhttp:server.impl.socket_options;

import std;
import asio;
import :server.options;

namespace mhttp::impl {

    /**
     * @brief Adapt native IPV6_V6ONLY to Asio's option protocol without including third-party headers.
     * @note The configured Asio module does not export ip::v6_only. Storage and size validation reuse
     *       its exported boolean option; only level/name come from the platform socket headers.
     */
    class IPv6Only : public asio::socket_base::reuse_address {
    public:
        /**
         * @brief Select IPv6-only or IPv4-mapped dual-stack mode before binding.
         * @param enabled Whether IPv4-mapped connections must be rejected.
         */
        explicit IPv6Only(bool enabled) : asio::socket_base::reuse_address{ enabled } {}

        /**
         * @brief Select the IPv6 option level.
         * @tparam Protocol Asio internet protocol type.
         * @param protocol Unused protocol descriptor.
         * @return Native IPPROTO_IPV6 value.
         */
        template <typename Protocol>
        auto level(Protocol const& protocol) const noexcept -> int { return IPPROTO_IPV6; }

        /**
         * @brief Select the IPv6-only option name.
         * @tparam Protocol Asio internet protocol type.
         * @param protocol Unused protocol descriptor.
         * @return Native IPV6_V6ONLY value.
         */
        template <typename Protocol>
        auto name(Protocol const& protocol) const noexcept -> int { return IPV6_V6ONLY; }
    };

#ifdef _WIN32
    /**
     * @brief Use Asio's boolean socket-option storage with Windows' exclusive-address option name.
     * @note Windows SO_REUSEADDR permits competing listeners to steal a live binding. Reserve it
     *       exclusively instead; the platform header supplies the option constant without copied values.
     */
    class ExclusiveAddressUse : public asio::socket_base::reuse_address {
    public:
        /**
         * @brief Enable exclusive reservation before binding.
         */
        ExclusiveAddressUse() : asio::socket_base::reuse_address{ true } {}

        /**
         * @brief Supply the Winsock option name for either IP protocol.
         * @tparam Protocol Asio internet protocol type.
         * @param protocol Unused protocol descriptor.
         * @return SO_EXCLUSIVEADDRUSE, interpreted at the inherited SOL_SOCKET level.
         */
        template <typename Protocol>
        auto name(Protocol const& protocol) const noexcept -> int { return SO_EXCLUSIVEADDRUSE; }
    };
#endif

    /**
     * @brief Configure an opened listener before bind without starting asynchronous operations.
     * @param acceptor Open acceptor, borrowed only during initialization.
     * @param protocol Protocol used to open this acceptor.
     * @param options Immutable server snapshot; reuse_address applies on POSIX, Windows reserves exclusively.
     * @return First native option failure, or a clear error code.
     */
    auto configure_listener_socket(
        asio::ip::tcp::acceptor& acceptor,
        asio::ip::tcp protocol,
        server::ServerOptions const& options
    ) -> asio::error_code {
        asio::error_code error;
#ifdef _WIN32
        acceptor.set_option(ExclusiveAddressUse{}, error);
#else
        acceptor.set_option(asio::socket_base::reuse_address{ options.reuse_address }, error);
#endif
        if (!error && protocol == asio::ip::tcp::v6()) {
            acceptor.set_option(IPv6Only{ options.ipv6_only }, error);
        }
        return error;
    }

    /**
     * @brief Configure a newly accepted socket before transferring it to the session strand.
     * @param socket Initialized connected socket, not concurrently accessed.
     * @param options Immutable server snapshot.
     * @return Native TCP_NODELAY failure, or a clear error code.
     */
    auto configure_session_socket(
        asio::ip::tcp::socket& socket,
        server::ServerOptions const& options
    ) -> asio::error_code {
        asio::error_code error;
        socket.set_option(asio::ip::tcp::no_delay{ options.tcp_no_delay }, error);
        return error;
    }

} // namespace mhttp::impl
