/**
 * @file config.cppm
 * @brief Declare the namespace aliases and vocabulary types shared by every mhttp partition.
 */
module;

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/string_type.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:config;

import std;
import nlohmann.json;

export namespace mhttp {

    namespace net = boost::asio;

    /**
     * @brief TCP protocol type used by acceptors, sockets, and endpoints.
     */
    using tcp = net::ip::tcp;

    /**
     * @brief Represent a successful value or a Boost.System error code.
     * @tparam T Success value type; void means no value is returned.
     */
    template <typename T = void>
    using Result = std::expected<T, boost::system::error_code>;

    /**
     * @brief HTTP request method.
     */
    using Verb = boost::beast::http::verb;

    /**
     * @brief HTTP response status code.
     */
    using Status = boost::beast::http::status;

    /**
     * @brief Well-known HTTP header field; unknown names use the string_view overloads.
     */
    using Field = boost::beast::http::field;

    /**
     * @brief JSON document type used by JSON bodies and NDJSON writers.
     */
    using Json = nlohmann::json;

} // namespace mhttp

// Beast aliases stay module-internal: only partitions of mhttp may name them.
namespace mhttp {

    namespace beast = boost::beast;
    namespace http = beast::http;

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Convert a Beast string view to std::string_view.
     * @param value View to convert; the result borrows the same bytes.
     * @return Equivalent standard view.
     */
    auto to_sv(beast::string_view value) noexcept -> std::string_view {
        return std::string_view{ value.data(), value.size() };
    }

} // namespace mhttp::detail
