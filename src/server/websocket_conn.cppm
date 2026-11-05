/**
 * @file websocket_conn.cppm
 * @brief Expose a server WebSocket connection and its session callbacks.
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:websocket_conn;

import std;
import :config;
import :request;
import :websocket_message;
import :headers;

export namespace mhttp {

    /**
     * @brief One server WebSocket session, owned by the server until its close callback finishes.
     *
     * Callbacks receive weak handles to avoid ownership cycles. Send, ping, pong, close, and
     * abort dispatch to the connection strand and may be called from any executor. Writes
     * queue with backpressure; the session owns the receive loop. Request access is strand-only.
     */
    class WebSocketConn : public std::enable_shared_from_this<WebSocketConn> {
    public:
        using WeakPtr = std::weak_ptr<WebSocketConn>;                                          ///< Non-owning session handle.
        using OpenHandler = std::function<net::awaitable<void>(WeakPtr)>;                      ///< Successful upgrade.
        using MessageHandler = std::function<net::awaitable<void>(WeakPtr, WebSocketMessage)>; ///< Message.
        using CloseHandler = std::function<net::awaitable<void>(WeakPtr)>;                     ///< Session ended.

        virtual ~WebSocketConn() = default;

        /**
         * @brief Return the strand on which callbacks and connection I/O run.
         */
        [[nodiscard]] virtual auto Executor() const noexcept -> net::any_io_executor = 0;

        /**
         * @brief Return a thread-safe snapshot of the upgraded connection's open state.
         */
        [[nodiscard]] virtual auto IsOpen() const noexcept -> bool = 0;

        /**
         * @brief Return a thread-safe snapshot of the peer's close code and reason.
         */
        [[nodiscard]] virtual auto CloseReason() const -> WebSocketCloseReason { return {}; }

        /**
         * @brief Borrow the handshake request, including path parameters and per-session Data().
         * @note Only access the request on Executor(); it lives as long as the connection handle.
         */
        [[nodiscard]] virtual auto HttpRequest() -> Request& = 0;
        [[nodiscard]] virtual auto HttpRequest() const -> Request const& = 0;

        /**
         * @brief Queue a complete text or binary message.
         * @param message Payload, valid until the operation completes.
         * @param ec Receives an I/O error or Error::NotInitialized after closing.
         */
        virtual auto AsyncSend(
            WebSocketMessage const& message,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;

        /**
         * @brief Queue an owned message from any thread.
         * @return Future of the send result; never block on it on an I/O thread.
         */
        virtual auto Send(WebSocketMessage message) -> std::future<boost::system::error_code> = 0;

        /**
         * @brief Queue a ping; payloads over 125 bytes fail with message_size.
         */
        virtual auto AsyncPing(
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;
        virtual auto Ping(std::string payload = {}) -> std::future<boost::system::error_code> = 0;

        /**
         * @brief Queue an unsolicited pong; payloads over 125 bytes fail with message_size.
         */
        virtual auto AsyncPong(
            std::string_view payload,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;
        virtual auto Pong(std::string payload = {}) -> std::future<boost::system::error_code> = 0;

        /**
         * @brief Send a normal close frame and wait at most five seconds for the peer.
         * @param reason Valid UTF-8; over 123 bytes fails with message_size without closing.
         * @param ec Receives the close error.
         */
        virtual auto AsyncClose(
            std::string_view reason,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;
        virtual auto Close(
            std::string reason = "normal"
        ) -> std::future<boost::system::error_code> = 0;

        /**
         * @brief Close with an explicit wire status and UTF-8 reason.
         * @note Implementations predating this overload fall back to their normal close method.
         */
        virtual auto AsyncClose(
            WebSocketCloseReason const& reason,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            co_await AsyncClose(reason.reason, ec);
        }

        /**
         * @brief Close with the default reason, "normal".
         */
        auto AsyncClose(boost::system::error_code& ec) -> net::awaitable<void> {
            co_await AsyncClose("normal", ec);
        }

        /**
         * @brief Close the socket immediately and cancel pending I/O and queued sends.
         */
        virtual auto AsyncAbort() -> net::awaitable<void> = 0;
        virtual auto Abort() -> std::future<void> = 0;
    };

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Callback set copied from a matched WebSocket route into its connection.
     */
    struct WsHandlerEntry {
        WebSocketConn::OpenHandler open;              ///< Runs once after a successful handshake.
        WebSocketConn::MessageHandler message;        ///< Runs sequentially for complete messages.
        WebSocketConn::CloseHandler close;            ///< Runs once after an established session ends.
        Headers response_headers;                     ///< Extra handshake response fields.
        std::function<void()> abort;                  ///< Interrupt a prepared upstream on forced abort.
        std::function<net::awaitable<void>()> finish; ///< Drain prepared resources even if upgrade fails.
        std::optional<std::uint64_t> message_limit;   ///< Optional route-specific receive bound.
    };

    /**
     * @brief WebSocket route with optional work performed before sending the downstream 101.
     */
    struct WsRouteEntry {
        using Prepare = std::function<net::awaitable<std::expected<WsHandlerEntry, Status>>(Request&)>;
        WsHandlerEntry handlers; ///< Ordinary local WebSocket callbacks.
        Prepare prepare;         ///< Per-session upstream handshake and callback factory.
    };

} // namespace mhttp::detail
