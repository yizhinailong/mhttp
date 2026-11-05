/**
 * @file reverse_proxy.cppm
 * @brief Stream HTTP exchanges to one upstream with downstream cancellation and bounded I/O.
 */
module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:reverse_proxy;

import std;
import :config;
import :error;
import :strings;
import :url;
import :headers;
import :async_event;
import :connection;
import :request;
import :response;
import :client;
import :client_pool;
import :lazy_request;
import :client_response;
import :proxy_common;
import :upstream_group;

export namespace mhttp {

    /**
     * @brief Immutable settings of a single-upstream HTTP reverse proxy route.
     */
    struct ReverseProxyConfig {
        bool strip_prefix{ true };                                                        ///< Replace the route prefix with the upstream base path.
        bool preserve_host{ false };                                                      ///< Forward the public Host instead of the upstream authority.
        std::size_t max_connections{ 100 };                                               ///< Pooled connections for this route.
        std::chrono::steady_clock::duration timeout{ std::chrono::seconds{ 30 } };        ///< Deadline per upstream I/O step.
        std::chrono::steady_clock::duration acquire_timeout{ std::chrono::seconds{ 3 } }; ///< Pool capacity wait.
        std::uint32_t header_limit{ 64 * 1024 };                                          ///< Maximum upstream response header bytes.
        std::uint64_t body_limit{ 1024ULL * 1024 * 1024 };                                ///< Maximum upstream response body bytes.
        UpstreamGroupConfig balancing;                                                    ///< Backend-list selection and passive recovery.
    };

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief The body reader removes chunk framing but cannot decode other transfer codings.
     */
    auto proxy_transfer_supported(Headers const& headers) -> bool {
        std::size_t count{ 0 };
        for (auto const field : headers) {
            if (field.name == Field::transfer_encoding) {
                ++count;
                if (!iequals(trim(field.value), "chunked")) {
                    return false;
                }
            }
        }
        return count <= 1;
    }

    /**
     * @brief Per-request relay state; every member is accessed on the downstream strand.
     */
    class ProxyExchange : public std::enable_shared_from_this<ProxyExchange> {
    private:
        std::shared_ptr<Connection> m_connection;       ///< Downstream transport.
        std::shared_ptr<client::HttpClientPool> m_pool; ///< Shared route pool.
        client::HttpClientPool::ClientHandle m_client;  ///< Exclusive upstream lease.
        UpstreamLease m_backend;                        ///< Active backend reservation through the full exchange.
        net::cancellation_signal m_acquire_cancel;      ///< Cancels a capacity waiter.
        net::cancellation_signal m_watch_cancel;        ///< Stops the non-consuming peer monitor.
        util::AsyncEvent m_watch_done;                  ///< Monitor completion before the next request may read.
        bool m_cancelled{ false };                      ///< Downstream aborted.
        bool m_watching{ false };                       ///< A monitor must be drained.
        bool m_watch_stopping{ false };                 ///< Cancellation latch before the monitor starts.

    public:
        /**
         * @brief Bind one exchange to its downstream and upstream pool.
         */
        ProxyExchange(
            std::shared_ptr<Connection> connection,
            std::shared_ptr<client::HttpClientPool> pool,
            UpstreamLease backend
        ) : m_connection{ std::move(connection) }, m_pool{ std::move(pool) }, m_backend{ std::move(backend) }, m_watch_done{ m_connection->Executor() } {}

        /**
         * @brief Cancel acquisition and close any borrowed upstream without blocking the strand.
         */
        auto Cancel() -> void {
            m_cancelled = true;
            m_acquire_cancel.emit(net::cancellation_type::all);
            if (m_client) {
                static_cast<void>(m_client->Close());
            }
        }

        /**
         * @brief Watch for downstream closure while no request-body read can overlap the monitor.
         */
        auto Watch() -> void {
            m_watching = true;
            net::co_spawn(m_connection->Executor(), [self = shared_from_this()]() -> net::awaitable<void> {
                boost::system::error_code ec;
                if (!self->m_watch_stopping) {
                    co_await self->m_connection->WatchDisconnect(self->m_watch_cancel.slot(), ec);
                }
                if (ec && ec != net::error::operation_aborted) {
                    self->Cancel();
                }
                self->m_watch_done.NotifyAll(); }, net::detached);
        }

        /**
         * @brief Drain the monitor and close a cancelled upstream before releasing the lease.
         */
        auto Finish() -> net::awaitable<void> {
            m_connection->SetAbortHandler({});
            if (m_watching) {
                m_watch_stopping = true;
                m_watch_cancel.emit(net::cancellation_type::all);
                static_cast<void>(co_await m_watch_done.Wait());
            }
            if (m_cancelled && m_client) {
                co_await m_client->AsyncClose();
            }
            m_client.Release();
            m_backend.Release();
        }

        /**
         * @brief Forward the body in bounded buffers, then relay the response incrementally.
         */
        auto Run(
            Request& request,
            Response& response,
            url::UrlInfo const& upstream,
            std::string const& target,
            ReverseProxyConfig const& config
        ) -> net::awaitable<void> {
            if (m_cancelled) {
                co_return;
            }
            if (request.IsBodyDone()) {
                Watch();
            }
            boost::system::error_code ec;
            m_client = co_await net::co_spawn(m_connection->Executor(), [this, &upstream, &config]() -> net::awaitable<client::HttpClientPool::ClientHandle> {
                co_await net::this_coro::throw_if_cancelled(false);
                if (m_cancelled) {
                    co_return client::HttpClientPool::ClientHandle{ net::error::operation_aborted };
                }
                co_return co_await m_pool->AsyncAcquire(upstream.host, upstream.EffectivePort(), url::Scheme::Plain, config.acquire_timeout); }, net::bind_cancellation_slot(m_acquire_cancel.slot(), net::redirect_error(net::use_awaitable, ec)));
            if (m_cancelled) {
                co_return;
            }
            if (ec || !m_client) {
                fail(response, ec ? ec : m_client.GetError());
                co_return;
            }
            m_client->SetHeaderLimit(config.header_limit);
            m_client->SetBodyLimit(config.body_limit);

            auto headers{ proxy_headers(request.Base()) };
            headers.Erase(Field::content_length);
            headers.Erase(Field::expect);
            headers.Erase("Forwarded");
            headers.Set(Field::host, config.preserve_host && !request[Field::host].empty() ? std::string{ request[Field::host] } : url::make_host_value(upstream.host, upstream.EffectivePort(), url::Scheme::Plain));
            headers.Set("X-Forwarded-For", request.GetClientIp().to_string());
            headers.Set("X-Forwarded-Host", request[Field::host]);
            headers.Set("X-Forwarded-Proto", request.IsTls() ? "https" : "http");
            headers.Set("X-Forwarded-Port", std::to_string(request.LocalEndpoint().port()));
            auto const length{ get_impl(request).ContentLength() };
            bool const chunked{ !length && !request.IsBodyDone() };
            if (length || !chunked) {
                headers.Set(Field::content_length, std::to_string(length.value_or(0)));
            }
            auto writer{ m_client->CreateLazyRequest() };
            co_await writer->WriteHeader(request.Method(), target, headers, chunked ? client::LazyRequest::Mode::Chunked : client::LazyRequest::Mode::Relay, ec);
            if (ec) {
                fail(response, ec, true);
                co_return;
            }
            std::array<char, 64 * 1024> buffer{};
            while (!request.IsBodyDone()) {
                auto const size{ co_await request.ReadSomeRaw(net::buffer(buffer), ec) };
                if (ec) {
                    response.SetErrorContent(ec == http::error::body_limit ? Status::payload_too_large : ec == beast::error::timeout ? Status::request_timeout :
                                                                                                                                       Status::bad_request);
                    get_impl(response).KeepAlive(false);
                    co_return;
                }
                if (size != 0) {
                    co_await writer->WriteBody(net::buffer(buffer.data(), size), !request.IsBodyDone(), ec);
                    if (ec) {
                        fail(response, ec, true);
                        co_return;
                    }
                }
            }
            if (!writer->IsBodyDone()) {
                co_await writer->WriteBody({}, false, ec);
                if (ec) {
                    fail(response, ec, true);
                    co_return;
                }
            }
            if (!m_watching) {
                Watch();
            }
            auto result{ co_await writer->ReadResponseLazy() };
            if (!result) {
                fail(response, result.error(), true);
                co_return;
            }
            if (m_cancelled) {
                co_return;
            }
            auto& received{ *result };
            if (received.ResultInt() == 101 || !proxy_transfer_supported(received.Base())) {
                fail(response, make_error_code(Error::ProtocolViolation), true);
                co_return;
            }
            m_backend.Success();
            auto forwarded{ proxy_headers(received.Base()) };
            forwarded.Erase(Field::content_length);
            auto& outgoing{ get_impl(response) };
            outgoing.DiscardBody();
            outgoing.Base().erase(Field::content_length);
            outgoing.Chunked(false);
            if (auto const body_length{ received.ContentLength() }) {
                forwarded.Set(Field::content_length, std::to_string(*body_length));
            }
            if (request.Method() == Verb::head || received.IsBodyDone()) {
                outgoing.Base().result(received.Result());
                outgoing.ReplaceFields(forwarded);
                co_return;
            }
            bool const reframe{ !received.ContentLength() && request.Version() >= 11 };
            if (!received.ContentLength() && !reframe) {
                outgoing.KeepAlive(false);
            }
            auto stream{ response.CreateStreamWriter() };
            co_await stream->WriteHeader(received.Result(), forwarded, reframe ? StreamWriter::Mode::Chunked : StreamWriter::Mode::Relay, ec);
            while (!ec && !received.IsBodyDone()) {
                auto const size{ co_await received.ReadSomeRaw(net::buffer(buffer), ec) };
                if (ec) {
                    fail(response, ec, true);
                    co_return;
                }
                if (!ec && size != 0) {
                    co_await stream->WriteBody(net::buffer(buffer.data(), size), !received.IsBodyDone(), ec);
                }
            }
            if (!ec && !outgoing.MessageDone()) {
                co_await stream->WriteBody({}, false, ec);
            }
            if (ec) {
                fail(response, ec);
            }
        }

    private:
        /**
         * @brief Send an error before headers; abort a partially sent response without a terminator.
         */
        auto fail(
            Response& response,
            boost::system::error_code ec,
            bool upstream_failure = false
        ) -> void {
            if (upstream_failure && !m_cancelled && ec != net::error::operation_aborted) {
                m_backend.Failure();
            }
            get_impl(response).KeepAlive(false);
            if (get_impl(response).HeaderSent()) {
                m_connection->Abort();
            } else {
                response.SetErrorContent(ec == net::error::timed_out || ec == beast::error::timeout || ec == Error::AcquireTimeout ? Status::gateway_timeout : Status::bad_gateway);
            }
            if (m_client) {
                static_cast<void>(m_client->Close());
            }
        }
    };

    /**
     * @brief Configured upstream group and connection pool shared by a prefix's routes.
     */
    class ReverseProxy {
    private:
        std::string m_prefix;                           ///< Normalized literal route prefix.
        std::shared_ptr<UpstreamGroup> m_upstreams;     ///< Validated HTTP backends and shared accounting.
        ReverseProxyConfig m_config;                    ///< Configuration snapshot.
        std::shared_ptr<client::HttpClientPool> m_pool; ///< Pool on the server executor.

    public:
        /**
         * @brief Validate and own a load-balanced HTTP backend list before registration.
         */
        ReverseProxy(
            net::any_io_executor executor,
            std::string prefix,
            std::vector<UpstreamBackend> upstreams,
            ReverseProxyConfig config
        ) : m_prefix{ proxy_prefix(std::move(prefix)) }, m_upstreams{ std::make_shared<UpstreamGroup>(std::move(upstreams), "http", config.balancing) }, m_config{ config } {
            if (config.max_connections == 0 || config.timeout <= std::chrono::steady_clock::duration::zero() || config.acquire_timeout <= std::chrono::steady_clock::duration::zero()) {
                throw std::invalid_argument{ "mhttp: reverse proxy requires a literal path prefix, an HTTP upstream URL, and positive limits" };
            }
            client::PoolParams params;
            params.max_size = config.max_connections;
            params.timeout_policy = client::TimeoutPolicy::Step;
            params.timeout = config.timeout;
            m_pool = std::make_shared<client::HttpClientPool>(executor, params);
        }

        /**
         * @brief Return the normalized prefix to register at the router.
         */
        [[nodiscard]] auto Prefix() const -> std::string const& { return m_prefix; }

        /**
         * @brief Relay one request and drain its cancellation observer before HTTP resumes.
         */
        auto Run(Request& request, Response& response) -> net::awaitable<void> {
            if (!proxy_transfer_supported(request.Base())) {
                response.SetErrorContent(Status::not_implemented);
                get_impl(response).KeepAlive(false);
                co_return;
            }
            if (!request[Field::upgrade].empty()) {
                response.SetErrorContent(Status::not_implemented);
                co_return;
            }
            auto backend{ m_upstreams->Acquire() };
            if (!backend) {
                response.SetErrorContent(Status::service_unavailable);
                co_return;
            }
            auto const& upstream{ backend->Url() };
            auto connection{ get_impl(request).GetConnection() };
            auto exchange{ std::make_shared<ProxyExchange>(connection, m_pool, std::move(*backend)) };
            connection->SetAbortHandler([weak = std::weak_ptr{ exchange }] { if (auto state{ weak.lock() }) { state->Cancel(); } });
            try {
                auto const target{ proxy_target(request, m_prefix, upstream.path, m_config.strip_prefix) };
                co_await exchange->Run(request, response, upstream, target, m_config);
            } catch (std::exception const&) {
                exchange->Cancel();
                response.SetErrorContent(Status::bad_gateway);
                get_impl(response).KeepAlive(false);
            }
            co_await exchange->Finish();
        }
    };

} // namespace mhttp::detail
