/**
 * @file server_peer.cppm
 * @brief Share raw loopback peer operations for bounded HTTP listener acceptance tests.
 */
module mhttp:test.server_peer;

import std;
import asio;
import :server.http_server;
import :test.transport;
import :test.session;

namespace mhttp::test {

    /**
     * @brief Connect an owned peer to a dynamically bound numeric loopback endpoint.
     * @param executor Independent fixture strand.
     * @param endpoint Owned server endpoint.
     * @return Connected socket, owned through every fixture await.
     */
    auto server_peer(
        asio::any_io_executor executor,
        Endpoint endpoint
    ) -> asio::awaitable<std::shared_ptr<asio::ip::tcp::socket>> {
        auto peer = std::make_shared<asio::ip::tcp::socket>(executor);
        co_await peer->async_connect({ asio::ip::make_address(endpoint.address), endpoint.port }, asio::use_awaitable);
        co_return peer;
    }

    /**
     * @brief Collect complete response bytes through FIN or an abrupt server stop.
     * @param peer Owned connected socket.
     * @return Owned bytes, including any complete response preceding stop.
     */
    auto receive_server(std::shared_ptr<asio::ip::tcp::socket> peer) -> asio::awaitable<std::string> {
        std::string wire;
        std::array<char, 257> buffer{};
        for (;;) {
            auto [error, count] = co_await peer->async_read_some(asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
            wire.append(buffer.data(), count);
            if (error == asio::error::eof || error == asio::error::connection_reset) { co_return wire; }
            if (error) { throw std::system_error{ error }; }
        }
    }

    /**
     * @brief Read one byte at a time to avoid consuming a future response before signaling idle state.
     * @param peer Owned raw socket.
     * @return One complete final response's owned bytes.
     */
    auto receive_one_server(std::shared_ptr<asio::ip::tcp::socket> peer) -> asio::awaitable<std::string> {
        detail::ResponseParser parser;
        std::string wire;
        char byte{};
        for (;;) {
            auto step = parser.Feed({});
            if (step.event == detail::ParseEvent::Complete) { co_return wire; }
            if (step.event == detail::ParseEvent::Failed) { throw std::runtime_error{ "invalid server response" }; }
            co_await asio::async_read(*peer, asio::buffer(&byte, 1), asio::use_awaitable);
            wire += byte;
            step = parser.Feed(std::span<char const>{ &byte, 1 });
            if (step.event == detail::ParseEvent::Failed) { throw std::runtime_error{ "invalid server response byte" }; }
        }
    }

    /**
     * @brief Send one owned request and await the server's closing response.
     * @param executor Fixture strand.
     * @param endpoint Owned server endpoint.
     * @param input Owned request(s).
     * @return Complete wire response(s).
     */
    auto server_exchange(
        asio::any_io_executor executor,
        Endpoint endpoint,
        std::string input
    ) -> asio::awaitable<std::string> {
        auto peer = co_await server_peer(executor, std::move(endpoint));
        co_await asio::async_write(*peer, asio::buffer(input), asio::use_awaitable);
        co_return co_await receive_server(std::move(peer));
    }

} // namespace mhttp::test
