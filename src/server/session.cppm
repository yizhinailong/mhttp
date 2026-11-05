/**
 * @file session.cppm
 * @brief Drive one accepted connection through a chain of protocol tasks.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/version.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:session;

import std;
import :config;
import :error;
import :strings;
import :logging;
import :form_data;
import :connection;
import :body_reader;
import :trusted_proxies;
import :request;
import :response;
import :router;
import :http_stream;

namespace mhttp {

    /**
     * @brief Server state that sessions read while handling requests.
     *
     * httplib hands each session the whole server implementation, which the server also
     * references. Module partitions cannot import each other cyclically, so the shared state
     * lives here and both HttpServer and Session hold it by shared_ptr.
     */
    struct ServerContext {
        detail::LoggerHolder logger{ "mhttp.server" };                                                   ///< Replaceable server logger.
        std::atomic<std::chrono::steady_clock::duration> read_timeout{ std::chrono::seconds{ 30 } };     ///< Per-read timeout, including keep-alive idle time.
        std::atomic<std::chrono::steady_clock::duration> write_timeout{ std::chrono::seconds{ 30 } };    ///< Per-write timeout.
        std::atomic<std::uint32_t> header_limit{ 64 * 1024 };                                            ///< Maximum request header bytes.
        std::atomic<std::uint64_t> body_limit{ 1024ULL * 1024 * 1024 };                                  ///< Maximum request body bytes (httplib default).
        std::atomic<std::shared_ptr<FormData::Param const>> form_data_params{ defaultFormDataParams() }; ///< Multipart limits snapshot.
        std::atomic<std::shared_ptr<detail::TrustedProxies const>> trusted_proxies;                     ///< Trusted proxies snapshot; null = none.
        Router router;                                                                                   ///< Routes; configured before Run().

        /**
         * @brief Return httplib's server defaults: 10 MiB per part, uploaded files removed.
         * @return Default multipart limits.
         */
        static auto defaultFormDataParams() -> std::shared_ptr<FormData::Param const> {
            return std::make_shared<FormData::Param>(FormData::Param{ .max_file_size = 10 * 1024 * 1024, .remove_uploaded_files = true });
        }
    };

    /**
     * @brief One accepted connection, running its protocol tasks on a dedicated strand.
     *
     * Each Task handles one protocol phase and returns the task for the next phase, or nullptr
     * to end the connection. Only HttpTask exists so far; httplib also chains TLS detection,
     * TLS handshake, WebSocket, and CONNECT-tunnel tasks here.
     */
    class Session : public std::enable_shared_from_this<Session> {
    public:
        /**
         * @brief One protocol phase of a connection.
         */
        class Task {
        public:
            using Ptr = std::shared_ptr<Task>;

            virtual ~Task() = default;

            /**
             * @brief Run this phase until it ends or hands the connection over.
             * @return Next phase, or nullptr to close the connection.
             */
            virtual auto Then() -> net::awaitable<Ptr> = 0;

            /**
             * @brief Interrupt this phase by closing its stream; runs on the connection strand.
             */
            virtual auto Abort() -> void = 0;
        };

        class HttpTask;

    private:
        net::any_io_executor m_executor;    ///< Connection strand; all task I/O runs here.
        Task::Ptr m_task;                   ///< Current phase; accessed only on m_executor.
        std::atomic<bool> m_abort{ false }; ///< Abort latch; set from any thread.

    public:
        /**
         * @brief Wrap an accepted socket and start in the HTTP phase.
         * @param executor Connection strand the socket is bound to.
         * @param socket Accepted socket.
         * @param context Shared server state.
         */
        Session(
            net::any_io_executor executor,
            tcp::socket&& socket,
            std::shared_ptr<ServerContext> context
        );

        /**
         * @brief Request the connection to end; idempotent and callable from any thread.
         * @note Never touches the socket from the calling thread: the close is posted to the
         *       connection strand so it serializes with in-flight I/O.
         */
        auto Abort() -> void;

        /**
         * @brief Run phases until one returns nullptr or Abort() is called.
         * @note Must be spawned on the connection strand.
         */
        auto Run() -> net::awaitable<void>;

        /**
         * @brief Return the connection strand.
         * @return Executor the session runs on.
         */
        [[nodiscard]] auto Executor() const noexcept -> net::any_io_executor {
            return m_executor;
        }
    };

    /**
     * @brief HTTP/1.1 phase: read a request header, route it, let the handler read the body and
     *        fill the response, write it, and repeat while the connection stays keep-alive.
     *
     * Implements detail::Connection for the request body reader and response writer. Those
     * operations co_spawn onto the connection strand, so a handler may start them from another
     * executor; request/response object state is still strand-only.
     */
    class Session::HttpTask : public Session::Task, public detail::Connection, public std::enable_shared_from_this<HttpTask> {
    private:
        std::shared_ptr<ServerContext> m_context; ///< Shared server state.
        HttpStream m_stream;                      ///< Connection stream.
        beast::flat_buffer m_buffer;              ///< Read buffer kept across keep-alive requests.

    public:
        /**
         * @brief Take over a connection stream.
         * @param stream Connection stream.
         * @param buffer Bytes already read from the stream, e.g. by protocol detection.
         * @param context Shared server state.
         */
        HttpTask(
            HttpStream&& stream,
            beast::flat_buffer&& buffer,
            std::shared_ptr<ServerContext> context
        ) : m_context{ std::move(context) }, m_stream{ std::move(stream) }, m_buffer{ std::move(buffer) } {}

        auto Then() -> net::awaitable<Ptr> override {
            boost::system::error_code ec;
            auto const remote{ m_stream.Socket().remote_endpoint(ec) };
            auto const local{ m_stream.Socket().local_endpoint(ec) };
            auto const peer{ std::format("({}:{} -> {}:{})", remote.address().to_string(), remote.port(), local.address().to_string(), local.port()) };
            auto& table{ route_table(m_context->router) };

            for (;;) {
                detail::BodyReader<true>::HeaderParser parser;
                parser.header_limit(m_context->header_limit.load());
                parser.body_limit(m_context->body_limit.load());

                co_await readHeader(parser, ec);
                if (ec) {
                    m_context->logger.GetLogger()->trace("read http header failed {}: {}", peer, ec.message());
                    co_await respondToHeaderError(ec);
                    co_return nullptr;
                }
                auto const start{ std::chrono::steady_clock::now() };

                auto req{ RequestImpl::Make(shared_from_this(), std::move(parser), local, remote, m_context->trusted_proxies.load(), *m_context->form_data_params.load()) };
                auto resp{ ResponseImpl::Create(req.Version(), get_impl(req).Message().keep_alive(), shared_from_this()) };
                auto& writer{ get_impl(resp) };

                std::optional<std::string> failure;
                auto handler_ms{ std::chrono::milliseconds::zero() };
                try {
                    auto const match{ table.PreRouting(req) };
                    if (!match.node) {
                        respondUnmatched(resp, match);
                        m_context->logger.GetLogger()->debug("{} {} {} {} not matched", req.MethodString(), req.Target(), resp.ResultInt(), peer);
                    } else {
                        bool body_ok{ true };
                        if (detail::iequals(req[Field::expect], "100-continue")) {
                            co_await writeContinue(req.Version(), ec);
                            if (ec) {
                                m_context->logger.GetLogger()->trace("write 100-continue failed {}: {}", peer, ec.message());
                                co_return nullptr;
                            }
                        }
                        if (!match.lazy) {
                            if (auto const read_ec{ co_await req.ReadBody() }) {
                                m_context->logger.GetLogger()->trace("read http body failed {}: {}", peer, read_ec.message());
                                if (!respondToBodyError(read_ec, resp)) {
                                    co_return nullptr;
                                }
                                body_ok = false;
                            }
                        }
                        if (body_ok) {
                            auto const handler_start{ std::chrono::steady_clock::now() };
                            co_await table.ProcessRouting(match, req, resp);
                            handler_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - handler_start);
                        }
                    }
                    co_await table.PostRouting(req, resp);
                    if (!req.IsBodyDone()) {
                        // Unread body bytes would be parsed as the next request.
                        writer.KeepAlive(false);
                    }
                } catch (std::exception const& e) {
                    failure = e.what();
                } catch (...) {
                    failure = "unknown exception";
                }
                if (failure) {
                    m_context->logger.GetLogger()->error("exception in handler for {} {} {}: {}", req.MethodString(), req.Target(), peer, *failure);
                    writer.KeepAlive(false);
                    resp.SetErrorContent(Status::internal_server_error);
                }

                if (!co_await writeResponse(req, resp, peer)) {
                    co_return nullptr;
                }
                auto const total_ms{ std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start) };
                m_context->logger.GetLogger()->debug("{} {} {} {} handler={}ms total={}ms", req.MethodString(), req.Target(), resp.ResultInt(), peer, handler_ms.count(), total_ms.count());

                if (!writer.KeepAlive()) {
                    co_await m_stream.AsyncClose();
                    co_return nullptr;
                }
            }
        }

        auto Abort() -> void override {
            boost::system::error_code ec;
            m_stream.Close(ec);
        }

        auto Executor() -> net::any_io_executor override {
            return m_stream.get_executor();
        }

        auto ReadSome(
            Parser& parser,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await net::co_spawn(
                m_stream.get_executor(),
                [&]() -> net::awaitable<void> {
                    m_stream.ExpiresAfter(m_context->read_timeout.load());
                    co_await http::async_read_some(m_stream, m_buffer, parser, net::redirect_error(net::use_awaitable, ec));
                    m_stream.ExpiresNever();
                },
                net::use_awaitable
            );
        }

        auto WriteHeader(
            Serializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await net::co_spawn(
                m_stream.get_executor(),
                [&]() -> net::awaitable<void> {
                    m_stream.ExpiresAfter(m_context->write_timeout.load());
                    co_await http::async_write_header(m_stream, serializer, net::redirect_error(net::use_awaitable, ec));
                    m_stream.ExpiresNever();
                },
                net::use_awaitable
            );
        }

        auto Write(
            Serializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await net::co_spawn(
                m_stream.get_executor(),
                [&]() -> net::awaitable<void> {
                    m_stream.ExpiresAfter(m_context->write_timeout.load());
                    co_await http::async_write(m_stream, serializer, net::redirect_error(net::use_awaitable, ec));
                    m_stream.ExpiresNever();
                },
                net::use_awaitable
            );
        }

    private:
        /**
         * @brief Read one request header, bounded by the read timeout.
         * @param parser Parser configured with the header and body limits.
         * @param ec Receives the read error, including end_of_stream when the peer closed.
         */
        auto readHeader(
            detail::BodyReader<true>::HeaderParser& parser,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            m_stream.ExpiresAfter(m_context->read_timeout.load());
            co_await http::async_read_header(m_stream, m_buffer, parser, net::redirect_error(net::use_awaitable, ec));
            m_stream.ExpiresNever();
        }

        /**
         * @brief Send "100 Continue" so the client starts sending the body.
         * @param version Request HTTP version.
         * @param ec Receives the write error.
         */
        auto writeContinue(
            unsigned version,
            boost::system::error_code& ec
        ) -> net::awaitable<void> {
            http::response<http::empty_body> response{ Status::continue_, version };
            m_stream.ExpiresAfter(m_context->write_timeout.load());
            co_await http::async_write(m_stream, response, net::redirect_error(net::use_awaitable, ec));
            m_stream.ExpiresNever();
        }

        /**
         * @brief Answer a malformed or oversized request header before closing.
         *
         * httplib closed silently; a status tells the client why. Peer closes and timeouts get
         * no answer.
         *
         * @param ec Header read error.
         */
        auto respondToHeaderError(boost::system::error_code const& ec) -> net::awaitable<void> {
            if (!isHttpError(ec) || ec == http::error::partial_message) {
                co_return;
            }
            auto const status{ ec == http::error::header_limit ? Status::request_header_fields_too_large
                               : ec == http::error::body_limit ? Status::payload_too_large
                                                               : Status::bad_request };
            http::response<http::string_body> response{ status, 11 };
            response.set(Field::server, BOOST_BEAST_VERSION_STRING);
            response.set(Field::content_type, "text/plain; charset=utf-8");
            response.body() = std::string{ detail::to_sv(http::obsolete_reason(status)) };
            response.keep_alive(false);
            response.prepare_payload();
            boost::system::error_code write_ec;
            m_stream.ExpiresAfter(m_context->write_timeout.load());
            co_await http::async_write(m_stream, response, net::redirect_error(net::use_awaitable, write_ec));
            m_stream.ExpiresNever();
            co_await m_stream.AsyncClose();
        }

        /**
         * @brief Turn a failed automatic body read into a 413/400 response.
         * @param ec Body read error.
         * @param resp Response to fill in.
         * @return False for network errors, where the connection should just close.
         */
        static auto respondToBodyError(
            boost::system::error_code const& ec,
            Response& resp
        ) -> bool {
            if (ec == http::error::body_limit) {
                resp.SetErrorContent(Status::payload_too_large);
            } else if (isHttpError(ec) || ec.category() == error_category()) {
                resp.SetErrorContent(Status::bad_request);
            } else {
                return false;
            }
            get_impl(resp).KeepAlive(false);
            return true;
        }

        /**
         * @brief Report whether an error comes from the HTTP parser (as opposed to the network).
         * @param ec Error to classify.
         * @return True for Beast HTTP errors other than end_of_stream.
         */
        static auto isHttpError(boost::system::error_code const& ec) -> bool {
            return ec.category() == make_error_code(http::error::end_of_stream).category() && ec != http::error::end_of_stream;
        }

        /**
         * @brief Answer an unmatched request: 405 with Allow when the path exists, else 404.
         * @param resp Response to fill in.
         * @param match Routing result with the allowed methods.
         */
        static auto respondUnmatched(
            Response& resp,
            detail::RouteTable::RouteMatch const& match
        ) -> void {
            if (!match.allows.empty()) {
                resp.Set(Field::allow, detail::join(match.allows, ","));
                resp.SetErrorContent(Status::method_not_allowed);
            } else {
                resp.SetErrorContent(Status::not_found);
            }
        }

        /**
         * @brief Write the response unless a stream writer already did.
         * @param req Request; HEAD responses send no body.
         * @param resp Response.
         * @param peer Connection label for log records.
         * @return False when the write failed and the connection must close.
         */
        auto writeResponse(
            Request& req,
            Response& resp,
            std::string_view peer
        ) -> net::awaitable<bool> {
            auto& writer{ get_impl(resp) };
            if (writer.HeaderSent()) {
                // A handler that streamed but did not finish leaves the connection mid-message.
                if (!writer.MessageDone()) {
                    writer.KeepAlive(false);
                }
                co_return true;
            }
            if (auto const ec{ co_await writer.WriteMessage(req.Method() == Verb::head) }) {
                m_context->logger.GetLogger()->trace("write http response failed {}: {}", peer, ec.message());
                co_return false;
            }
            co_return true;
        }
    };

    Session::Session(
        net::any_io_executor executor,
        tcp::socket&& socket,
        std::shared_ptr<ServerContext> context
    ) : m_executor{ std::move(executor) },
        m_task{ std::make_shared<HttpTask>(HttpStream{ HttpStream::PlainStream{ std::move(socket) } }, beast::flat_buffer{}, std::move(context)) } {}

    auto Session::Abort() -> void {
        if (m_abort.exchange(true)) {
            return;
        }
        net::post(m_executor, [self = shared_from_this()]() {
            if (auto const task{ self->m_task }) {
                task->Abort();
            }
        });
    }

    auto Session::Run() -> net::awaitable<void> {
        while (!m_abort.load()) {
            auto const task{ m_task };
            if (!task) {
                break;
            }
            m_task = co_await task->Then();
        }
        m_task.reset();
    }

} // namespace mhttp
