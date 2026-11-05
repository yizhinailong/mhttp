/**
 * @file connection.cppm
 * @brief Declare the connection interface through which message bodies are read and written.
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:connection;

import std;
import :config;

namespace mhttp::detail {

    /**
     * @brief Connection-level I/O used by lazy body readers and body writers.
     *
     * A server connection reads requests and writes responses; a client connection does the
     * opposite. Message objects only see this interface, which keeps their partitions free of a
     * cyclic import on the session or client that owns the socket. Every operation is spawned
     * onto the connection strand and applies the owner's timeouts, so it may be started from any
     * executor; message object state is still strand-only.
     *
     * @tparam IsServer True for the server side (reads requests), false for the client side.
     */
    template <bool IsServer>
    class BasicConnection {
    public:
        using Parser = http::parser<IsServer, http::buffer_body>;          ///< Incoming message parser.
        using Serializer = http::serializer<!IsServer, http::buffer_body>; ///< Outgoing message serializer.

        virtual ~BasicConnection() = default;

        /**
         * @brief Return the connection strand.
         * @return Executor that serializes the connection's I/O.
         */
        [[nodiscard]] virtual auto Executor() -> net::any_io_executor = 0;

        /**
         * @brief Read more of the incoming body into the parser's attached buffer.
         * @param parser Parser with a buffer_body buffer attached.
         * @param ec Receives the read error; need_buffer means the buffer is full.
         */
        virtual auto ReadSome(
            Parser& parser,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;

        /**
         * @brief Write only the outgoing header.
         * @param serializer Serializer of the outgoing message.
         * @param ec Receives the write error.
         */
        virtual auto WriteHeader(
            Serializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;

        /**
         * @brief Write until the attached body buffer is consumed or the message is done.
         * @param serializer Serializer of the outgoing message.
         * @param ec Receives the write error; need_buffer means more body is expected.
         */
        virtual auto Write(
            Serializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;
    };

    using ClientConnection = BasicConnection<false>; ///< Client side: writes requests, reads responses.

    /**
     * @brief Server transport hooks used to cancel a streaming relay with its downstream.
     */
    class Connection : public BasicConnection<true> {
    public:
        /**
         * @brief Install or clear the current request's abort hook; connection strand only.
         */
        virtual auto SetAbortHandler(std::function<void()> handler) -> void = 0;

        /**
         * @brief Observe peer closure after the request body; never consume pipelined bytes.
         * @note Success means another request is readable; errors indicate closure or cancellation.
         */
        virtual auto WatchDisconnect(
            net::cancellation_slot slot,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;

        /**
         * @brief Abort the transport and its relay; connection strand only.
         */
        virtual auto Abort() -> void = 0;
    };

} // namespace mhttp::detail
