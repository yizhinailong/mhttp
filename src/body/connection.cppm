/**
 * @file connection.cppm
 * @brief Declare the connection interface through which request and response bodies do I/O.
 */
module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:connection;

import std;
import :config;

namespace mhttp::detail {

    /**
     * @brief Connection-level I/O used by lazy request bodies and response writers.
     *
     * The HTTP session implements it. Request and response objects only see this interface,
     * which keeps their partitions free of a cyclic import on the session. Every operation is
     * spawned onto the connection strand and applies the server's read or write timeout, so it
     * may be started from any executor; object state is still strand-only.
     */
    class Connection {
    public:
        using RequestParser = http::request_parser<http::buffer_body>;
        using ResponseSerializer = http::response_serializer<http::buffer_body>;

        virtual ~Connection() = default;

        /**
         * @brief Return the connection strand.
         * @return Executor that serializes the connection's I/O.
         */
        [[nodiscard]] virtual auto Executor() -> net::any_io_executor = 0;

        /**
         * @brief Read more of the request body into the parser's attached buffer.
         * @param parser Parser with a buffer_body buffer attached.
         * @param ec Receives the read error; need_buffer means the buffer is full.
         */
        virtual auto ReadSome(
            RequestParser& parser,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;

        /**
         * @brief Write only the response header.
         * @param serializer Serializer of the response.
         * @param ec Receives the write error.
         */
        virtual auto WriteHeader(
            ResponseSerializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;

        /**
         * @brief Write until the attached body buffer is consumed or the message is done.
         * @param serializer Serializer of the response.
         * @param ec Receives the write error; need_buffer means more body is expected.
         */
        virtual auto Write(
            ResponseSerializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> = 0;
    };

} // namespace mhttp::detail
