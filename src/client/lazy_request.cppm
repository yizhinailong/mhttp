/**
 * @file lazy_request.cppm
 * @brief Provide a client request whose body the caller streams chunk by chunk.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

export module mhttp:lazy_request;

import std;
import :config;
import :error;
import :headers;
import :body_writer;
import :client_impl;
import :client_response;

export namespace mhttp::client {

    /**
     * @brief Incremental request writer bound to one client; see HttpClient::CreateLazyRequest().
     *
     * WriteHeader() admits the request on the client (one request at a time), WriteBody()
     * streams the body, and ReadResponse()/ReadResponseLazy() end the exchange. While the
     * object is alive the client is considered busy, even between exchanges, because it may
     * start writing again at any time; release it before returning a pooled client. Destroying
     * it in the middle of an exchange closes the connection. Do not overlap calls.
     */
    class LazyRequest {
    public:
        /**
         * @brief How the body is framed.
         */
        enum class Mode {
            Relay,   ///< Keep the framing given in the headers (e.g. an explicit Content-Length).
            Chunked, ///< Use chunked transfer coding.
        };

    private:
        std::shared_ptr<detail::ClientImpl> m_client; ///< Client the request is sent on.
        detail::BodyWriter<true> m_writer;            ///< Message and serializer.
        detail::ExchangeGuard m_exchange;             ///< Admitted exchange until the response header.

    public:
        /**
         * @brief Bind to a client; created by HttpClient::CreateLazyRequest().
         * @param client Client state.
         */
        explicit LazyRequest(
            std::shared_ptr<detail::ClientImpl> client
        ) : m_client{ std::move(client) } {
            m_client->AddWriter();
        }

        LazyRequest(LazyRequest const&) = delete;
        auto operator=(LazyRequest const&) -> LazyRequest& = delete;

        ~LazyRequest() {
            m_client->RemoveWriter();
        }

        /**
         * @brief Report whether the current request's body has been fully serialized.
         */
        [[nodiscard]] auto IsBodyDone() const -> bool {
            return m_writer.MessageDone();
        }

        /**
         * @brief Admit the request and send its header.
         * @param method Method.
         * @param target Origin-form target.
         * @param headers Fields to send; Host is filled in when absent.
         * @param mode Body framing.
         * @throws boost::system::system_error On failure.
         */
        auto WriteHeader(
            Verb method,
            std::string_view target,
            Headers const& headers,
            Mode mode
        ) -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await WriteHeader(method, target, headers, mode, ec);
            if (ec) {
                throw boost::system::system_error{ ec };
            }
        }

        /**
         * @brief Admit the request and send its header.
         * @param method Method.
         * @param target Origin-form target.
         * @param headers Fields to send; Host is filled in when absent.
         * @param mode Body framing.
         * @param ec Receives Error::AlreadyInUse while the client is busy, or the write error.
         */
        auto WriteHeader(
            Verb method,
            std::string_view target,
            Headers const& headers,
            Mode mode,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            // A second header before the previous response was read would pipeline two
            // requests on one connection.
            if (m_exchange.Active()) {
                ec = Error::AlreadyInUse;
                co_return;
            }
            if (ec = m_exchange.Begin(m_client); ec) {
                co_return;
            }
            m_writer.Reset();
            auto& base{ m_writer.Base() };
            base.clear();
            base.method(method);
            base.target(target);
            base.version(11);
            base.set(Field::user_agent, BOOST_BEAST_VERSION_STRING);
            for (auto const& header : headers) {
                base.insert(header.name_string, header.value);
            }
            if (base.find(Field::host) == base.end()) {
                base.set(Field::host, m_client->HostValue());
            }
            // httplib forced keep-alive here, overriding a caller's "Connection: close".
            if (base.find(Field::connection) == base.end()) {
                m_writer.KeepAlive(true);
            }
            if (mode == Mode::Chunked) {
                m_writer.Chunked(true);
            }
            m_writer.Attach(m_client);
            m_writer.PrepareForSend();
            ec = co_await m_writer.BeginStream();
            if (ec) {
                m_exchange = detail::ExchangeGuard{};
            }
        }

        /**
         * @brief Send one body chunk.
         * @param data Bytes to send.
         * @param more False for the last chunk.
         * @throws boost::system::system_error On failure.
         */
        auto WriteBody(net::const_buffer data, bool more) -> net::awaitable<void> {
            boost::system::error_code ec;
            co_await WriteBody(data, more, ec);
            if (ec) {
                throw boost::system::system_error{ ec };
            }
        }

        /**
         * @brief Send one body chunk.
         * @param data Bytes to send.
         * @param more False for the last chunk.
         * @param ec Receives the write error; invalid_argument outside an exchange.
         */
        auto WriteBody(
            net::const_buffer data,
            bool more,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            if (!m_exchange.Active()) {
                ec = boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
                co_return;
            }
            ec = co_await m_writer.WriteRaw(data, more);
        }

        /**
         * @brief Read the response header and end the exchange; the body stays unread.
         * @return Response, Error::NotInitialized without a sent header, or the read error.
         * @note If the request body was not finished, the connection closes after this response.
         */
        auto ReadResponseLazy() -> net::awaitable<mhttp::Result<Response>> {
            if (!m_exchange.Active()) {
                co_return std::unexpected{ make_error_code(Error::NotInitialized) };
            }
            auto header{ co_await m_client->ReadResponseHeader(m_writer.Base().method()) };
            if (!header) {
                m_exchange = detail::ExchangeGuard{};
                co_return std::unexpected{ header.error() };
            }
            auto response{ ResponseImpl::Create(m_client, std::move(*header), !m_writer.MessageDone()) };
            m_exchange.Complete(!response.IsBodyDone());
            co_return response;
        }

        /**
         * @brief Read the whole response and end the exchange.
         * @return Response with a materialized body, or the read error.
         */
        auto ReadResponse() -> net::awaitable<mhttp::Result<Response>> {
            auto result{ co_await ReadResponseLazy() };
            if (!result) {
                co_return result;
            }
            if (auto const ec{ co_await result->ReadBody() }) {
                co_return std::unexpected{ ec };
            }
            co_return result;
        }
    };

} // namespace mhttp::client
