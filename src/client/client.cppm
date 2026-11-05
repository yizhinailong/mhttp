/**
 * @file client.cppm
 * @brief Provide HttpClient: one connection, request shorthands, redirects, and downloads.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

export module mhttp:client;

import std;
import :config;
import :error;
import :strings;
import :url;
import :tls;
import :query_params;
import :headers;
import :body_writer;
import :client_impl;
import :client_request;
import :client_response;
import :lazy_request;

export namespace mhttp::client {

    /**
     * @brief How much of the response a send reads before returning.
     */
    enum class BodyMode {
        Eager, ///< Read the whole body; the client is free again when the send returns.
        Lazy,  ///< Read only the header; read the body before the next request.
    };

    /**
     * @brief HTTP/1.1 client owning one connection (port of httplib::client::http_client).
     *
     * The connection opens on the first request and reopens after a close. Requests run one at
     * a time: a second request while one is in flight, or while a lazy response body is unread,
     * fails with Error::AlreadyInUse. A request that fails because the server closed a reused
     * idle connection is sent once more on a new connection. Settings are atomic and may change
     * from any thread. HTTPS verifies the server's certificate chain and host by default.
     */
    class HttpClient {
    private:
        std::shared_ptr<detail::ClientImpl> m_impl; ///< Connection state, shared with responses.

    public:
        /**
         * @brief Create a client for a host and port.
         * @param executor Executor the client's strand is made from.
         * @param host Host name or address; also the Host header.
         * @param port Port.
         * @param scheme Transport.
         */
        HttpClient(
            net::any_io_executor const& executor,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        );

        HttpClient(
            net::io_context& context,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme = url::Scheme::Plain
        );

        /**
         * @brief Create a client for the origin of a URL; its path is ignored.
         * @param executor Executor the client's strand is made from.
         * @param base_url "http://host[:port]" or "https://host[:port]".
         * @throws boost::system::system_error Error::InvalidUrl for other input.
         */
        HttpClient(net::any_io_executor const& executor, std::string_view base_url);
        HttpClient(net::io_context& context, std::string_view base_url);

        HttpClient(HttpClient const&) = delete;
        auto operator=(HttpClient const&) -> HttpClient& = delete;

        ~HttpClient() = default;

        auto SetTimeoutPolicy(TimeoutPolicy policy) -> void;

        /**
         * @brief Set the timeout used by the Overall and Step policies.
         * @param timeout Timeout; default 30 s.
         */
        auto SetTimeout(std::chrono::steady_clock::duration timeout) -> void;

        /**
         * @brief Select shared trust configuration for future HTTPS connections and redirects.
         * @param context Context from TlsClientContext::Create(); null uses system roots.
         * @note A changed context reconnects before the next request; in-flight requests finish.
         */
        auto SetTlsContext(std::shared_ptr<TlsClientContext> context) -> void;

        [[nodiscard]] auto Host() const -> std::string_view;
        [[nodiscard]] auto Port() const -> std::uint16_t;
        [[nodiscard]] auto GetScheme() const -> url::Scheme;

        [[nodiscard]] auto Logger() const -> std::shared_ptr<spdlog::logger>;
        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void;

        /**
         * @brief Set how many redirects to follow.
         * @param count Limit; 0 (the default) returns 3xx responses unchanged.
         */
        auto SetMaxRedirects(int count) -> void;

        /**
         * @brief Limit response header size; larger headers fail with http::error::header_limit.
         * @param limit Bytes; default 64 KiB.
         */
        auto SetHeaderLimit(std::uint32_t limit) -> void;

        /**
         * @brief Limit response body size; larger bodies fail with http::error::body_limit.
         * @param limit Bytes; default 1 GiB.
         */
        auto SetBodyLimit(std::uint64_t limit) -> void;

        /**
         * @brief Cap the response (download) throughput.
         * @param bytes_per_second Limit; 0 means unlimited.
         */
        auto SetDownloadRateLimit(std::uint64_t bytes_per_second) -> void;

        /**
         * @brief Cap the request (upload) throughput.
         * @param bytes_per_second Limit; 0 means unlimited.
         */
        auto SetUploadRateLimit(std::uint64_t bytes_per_second) -> void;

        /**
         * @brief Close the connection from any thread.
         * @return Future ready once closed; waiting on it from the client's I/O thread deadlocks.
         */
        auto Close() -> std::future<void>;
        auto AsyncClose() -> net::awaitable<void>;

        /**
         * @brief Report whether a connection is open.
         * @return Snapshot; reliable only while the client is idle.
         */
        [[nodiscard]] auto IsOpen() const -> bool;

        /**
         * @brief Report whether the client is busy.
         * @return True with a live LazyRequest, a request in flight, or an unread response body.
         */
        [[nodiscard]] auto HasActiveSession() const -> bool;

        /**
         * @brief Probe whether the idle connection is still usable; call only while idle.
         * @return Future of the probe result.
         */
        auto IsAlive() const -> std::future<bool>;
        auto AsyncIsAlive() const -> net::awaitable<bool>;

        /**
         * @brief Create a request whose body the caller streams.
         * @return New writer bound to this client.
         */
        [[nodiscard]] auto CreateLazyRequest() -> std::shared_ptr<LazyRequest>;

        /**
         * @brief Send a request and read its response, following redirects up to the limit.
         * @param request Request; Host is added when absent; the body is replayed on redirects.
         * @param mode How much of the response to read.
         * @return Response, or the connect / write / read error, Error::AlreadyInUse,
         *         Error::TooManyRedirects, or Error::InvalidUrl for a bad Location.
         */
        auto AsyncSendRequest(
            Request& request,
            BodyMode mode = BodyMode::Eager
        ) -> net::awaitable<Result<Response>>;

        auto AsyncSendRequest(
            Request&& request,
            BodyMode mode = BodyMode::Eager
        ) -> net::awaitable<Result<Response>>;

        auto AsyncGet(
            std::string_view path,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncHead(
            std::string_view path,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPost(
            std::string_view path,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPut(
            std::string_view path,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPatch(
            std::string_view path,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncDelete(
            std::string_view path,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncOptions(
            std::string_view path,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPost(
            std::string_view path,
            std::string_view body,
            std::string_view content_type,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPost(
            std::string_view path,
            Json const& body,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPut(
            std::string_view path,
            std::string_view body,
            std::string_view content_type,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPut(
            std::string_view path,
            Json const& body,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPatch(
            std::string_view path,
            std::string_view body,
            std::string_view content_type,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        auto AsyncPatch(
            std::string_view path,
            Json const& body,
            QueryParams const& params = {},
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;

        /**
         * @brief Stream a response body into a file, following redirects.
         * @param method Method.
         * @param path Request target.
         * @param save_path Destination; removed again if the body read fails.
         * @param headers Extra fields.
         * @return Response whose body went to the file (any status), or the error.
         */
        auto AsyncDownload(
            Verb method,
            std::string_view path,
            std::filesystem::path const& save_path,
            Headers const& headers = {}
        ) -> net::awaitable<Result<Response>>;
    };

} // namespace mhttp::client

namespace mhttp::detail {

    /**
     * @brief Report whether a status asks the client to follow Location.
     * @param status Response status.
     * @return True for 301, 302, 303, 307, and 308.
     */
    auto is_redirect(Status status) -> bool {
        return status == Status::moved_permanently || status == Status::found || status == Status::see_other || status == Status::temporary_redirect || status == Status::permanent_redirect;
    }

    /**
     * @brief Report whether a failure looks like the server closed an idle connection.
     * @param ec Write or header-read error.
     * @return True for EOF before any response byte, reset, abort, and broken pipe.
     */
    auto is_retryable(boost::system::error_code const& ec) -> bool {
        return ec == http::error::end_of_stream || ec == net::error::eof || ec == net::error::connection_reset || ec == net::error::connection_aborted || ec == net::error::broken_pipe;
    }

    /**
     * @brief Report whether a URL scheme is one the client speaks.
     * @param scheme Scheme name.
     * @return True for http and https, case-insensitively.
     */
    auto is_http_scheme(std::string_view scheme) -> bool {
        return iequals(scheme, "http") || iequals(scheme, "https");
    }

    /**
     * @brief Report whether a reference starts with "scheme://".
     * @param reference Location value.
     * @return True for absolute URLs of any scheme; false for relative references.
     */
    auto is_absolute_url(std::string_view reference) -> bool {
        auto const separator{ reference.find("://") };
        if (separator == std::string_view::npos || separator == 0) {
            return false;
        }
        auto const scheme{ reference.substr(0, separator) };
        return std::isalpha(static_cast<unsigned char>(scheme.front())) != 0 && std::ranges::all_of(scheme, [](char c) {
                   return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '-' || c == '.';
               });
    }

    /**
     * @brief Send a request once: admit it, write it, and read the response header.
     *
     * When a reused connection fails before any response byte (the server closed it while
     * idle), the request is replayed once on a new connection. httplib only retried failed
     * header writes, which rarely fail on a half-closed socket.
     *
     * @param client Client to send on; must be running on its strand.
     * @param request Request writer.
     * @return Response with an unread body, or the error.
     */
    auto send_once(
        std::shared_ptr<ClientImpl> const& client,
        BodyWriter<true>& request
    ) -> net::awaitable<Result<client::Response>> {
        ExchangeGuard exchange;
        if (auto const ec{ exchange.Begin(client) }) {
            co_return std::unexpected{ ec };
        }
        auto& base{ request.Base() };
        if (base.find(Field::host) == base.end()) {
            base.set(Field::host, client->HostValue());
        }
        request.Attach(client);
        for (int attempt{ 0 };; ++attempt) {
            if (!request.PrepareForSend()) {
                co_return std::unexpected{ boost::system::errc::make_error_code(boost::system::errc::invalid_argument) };
            }
            auto ec{ co_await request.WriteMessage() };
            bool const reused{ client->AttemptReused() };
            if (!ec) {
                auto header{ co_await client->ReadResponseHeader(base.method()) };
                if (header) {
                    auto response{ client::ResponseImpl::Create(client, std::move(*header)) };
                    exchange.Complete(!response.IsBodyDone());
                    co_return response;
                }
                ec = header.error();
            }
            if (attempt > 0 || !reused || !is_retryable(ec)) {
                co_return std::unexpected{ ec };
            }
            client->Logger().GetLogger()->debug("{} {} failed on a reused connection ({}), retrying", to_sv(base.method_string()), to_sv(base.target()), ec.message());
            co_await client->AsyncClose();
        }
    }

    /**
     * @brief Send a request and follow redirects.
     *
     * Location may be absolute, network-path ("//host/x"), or relative (RFC 3986). 303 turns any
     * method but GET/HEAD into a body-less GET, and 301/302 do so for POST only (Fetch
     * standard; httplib rewrote every method but HEAD). A redirect to another origin continues on
     * a new client with the same settings, after removing Authorization, Proxy-Authorization,
     * Cookie, Cookie2, and the old Host (httplib kept the old Host).
     *
     * @param client Client to start on; must be running on its strand.
     * @param request Request writer; its target and method are updated per hop.
     * @param max_redirects Remaining redirect budget.
     * @param exhausted_is_error Whether a redirect beyond a zero budget is an error rather than
     *        the result (true after a cross-origin hop).
     * @return Final response, or the error.
     */
    auto send_with_redirects(
        std::shared_ptr<ClientImpl> client,
        BodyWriter<true>& request,
        int max_redirects,
        bool exhausted_is_error
    ) -> net::awaitable<Result<client::Response>> {
        for (int hop{ 0 };; ++hop) {
            auto result{ co_await send_once(client, request) };
            if (!result) {
                co_return result;
            }
            auto const status{ result->Result() };
            std::string const location{ (*result)[Field::location] };
            if (!is_redirect(status) || location.empty()) {
                co_return result;
            }
            if (hop >= max_redirects && max_redirects <= 0 && !exhausted_is_error) {
                co_return result;
            }
            // Discard the redirect body so the connection can be reused; a failed drain closes
            // it when the response is released.
            static_cast<void>(co_await result->Drain());
            *result = client::Response{};
            if (hop >= max_redirects) {
                co_return std::unexpected{ make_error_code(Error::TooManyRedirects) };
            }

            auto& base{ request.Base() };
            client->Logger().GetLogger()->trace("redirect {} -> {}", to_sv(base.target()), location);
            auto const method{ base.method() };
            if ((status == Status::see_other && method != Verb::get && method != Verb::head) || ((status == Status::moved_permanently || status == Status::found) && method == Verb::post)) {
                base.method(Verb::get);
                request.Reset();
                base.erase(Field::content_type);
            }

            std::string reference{ location };
            if (reference.starts_with("//")) {
                reference = std::format("{}:{}", url::to_string(client->GetScheme()), reference);
            }
            if (!is_absolute_url(reference)) {
                base.target(url::resolve(to_sv(base.target()), reference));
                continue;
            }
            auto const parsed{ url::parse_url(reference) };
            if (!parsed || !is_http_scheme(parsed->scheme)) {
                co_return std::unexpected{ make_error_code(Error::InvalidUrl) };
            }
            base.target(parsed->Target());
            auto const port{ parsed->EffectivePort() };
            auto const scheme{ parsed->Transport() };
            if (iequals(parsed->host, client->Host()) && port == client->Port() && scheme == client->GetScheme()) {
                continue;
            }
            for (auto const field : { Field::authorization, Field::proxy_authorization, Field::cookie, Field::host }) {
                base.erase(field);
            }
            base.erase("Cookie2");
            auto next{ std::make_shared<ClientImpl>(client->BaseExecutor(), parsed->host, port, scheme) };
            next->CopySettingsFrom(*client);
            co_return co_await net::co_spawn(next->Executor(), send_with_redirects(next, request, max_redirects - hop - 1, true), net::use_awaitable);
        }
    }

} // namespace mhttp::detail

namespace mhttp::client {

    HttpClient::HttpClient(
        net::any_io_executor const& executor,
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) : m_impl{ std::make_shared<detail::ClientImpl>(executor, host, port, scheme) } {}

    HttpClient::HttpClient(
        net::io_context& context,
        std::string_view host,
        std::uint16_t port,
        url::Scheme scheme
    ) : HttpClient{ context.get_executor(), host, port, scheme } {}

    HttpClient::HttpClient(net::any_io_executor const& executor, std::string_view base_url) {
        auto const parsed{ url::parse_url(base_url) };
        if (!parsed || !detail::is_http_scheme(parsed->scheme)) {
            throw boost::system::system_error{ make_error_code(Error::InvalidUrl), std::format("invalid url: {}", base_url) };
        }
        m_impl = std::make_shared<detail::ClientImpl>(executor, parsed->host, parsed->EffectivePort(), parsed->Transport());
    }

    HttpClient::HttpClient(
        net::io_context& context,
        std::string_view base_url
    ) : HttpClient{ context.get_executor(), base_url } {}

    auto HttpClient::SetTimeoutPolicy(TimeoutPolicy policy) -> void {
        m_impl->SetTimeoutPolicy(policy);
    }

    auto HttpClient::SetTimeout(std::chrono::steady_clock::duration timeout) -> void {
        m_impl->SetTimeout(timeout);
    }

    auto HttpClient::SetTlsContext(std::shared_ptr<TlsClientContext> context) -> void {
        m_impl->SetTlsContext(std::move(context));
    }

    auto HttpClient::Host() const -> std::string_view {
        return m_impl->Host();
    }

    auto HttpClient::Port() const -> std::uint16_t {
        return m_impl->Port();
    }

    auto HttpClient::GetScheme() const -> url::Scheme {
        return m_impl->GetScheme();
    }

    auto HttpClient::Logger() const -> std::shared_ptr<spdlog::logger> {
        return m_impl->Logger().GetLogger();
    }

    auto HttpClient::SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
        m_impl->SetLogger(std::move(logger));
    }

    auto HttpClient::SetMaxRedirects(int count) -> void {
        m_impl->SetMaxRedirects(count);
    }

    auto HttpClient::SetHeaderLimit(std::uint32_t limit) -> void {
        m_impl->SetHeaderLimit(limit);
    }

    auto HttpClient::SetBodyLimit(std::uint64_t limit) -> void {
        m_impl->SetBodyLimit(limit);
    }

    auto HttpClient::SetDownloadRateLimit(std::uint64_t bytes_per_second) -> void {
        m_impl->SetRateLimits(bytes_per_second, m_impl->UploadRateLimit());
    }

    auto HttpClient::SetUploadRateLimit(std::uint64_t bytes_per_second) -> void {
        m_impl->SetRateLimits(m_impl->DownloadRateLimit(), bytes_per_second);
    }

    auto HttpClient::Close() -> std::future<void> {
        return m_impl->Close();
    }

    auto HttpClient::AsyncClose() -> net::awaitable<void> {
        co_await m_impl->AsyncClose();
    }

    auto HttpClient::IsOpen() const -> bool {
        return m_impl->IsOpen();
    }

    auto HttpClient::HasActiveSession() const -> bool {
        return m_impl->HasActiveSession();
    }

    auto HttpClient::IsAlive() const -> std::future<bool> {
        auto const impl{ m_impl };
        return net::co_spawn(
            impl->Executor(),
            [impl]() -> net::awaitable<bool> { co_return co_await impl->AsyncIsAlive(); },
            net::use_future
        );
    }

    auto HttpClient::AsyncIsAlive() const -> net::awaitable<bool> {
        co_return co_await m_impl->AsyncIsAlive();
    }

    auto HttpClient::CreateLazyRequest() -> std::shared_ptr<LazyRequest> {
        return std::make_shared<LazyRequest>(m_impl);
    }

    auto HttpClient::AsyncSendRequest(
        Request& request,
        BodyMode mode
    ) -> net::awaitable<Result<Response>> {
        auto const impl{ m_impl };
        auto result{ co_await net::co_spawn(impl->Executor(), detail::send_with_redirects(impl, get_impl(request), impl->MaxRedirects(), false), net::use_awaitable) };
        if (result && mode == BodyMode::Eager) {
            if (auto const ec{ co_await result->ReadBody() }) {
                co_return std::unexpected{ ec };
            }
        }
        co_return result;
    }

    auto HttpClient::AsyncSendRequest(
        Request&& request,
        BodyMode mode
    ) -> net::awaitable<Result<Response>> {
        Request held{ std::move(request) };
        co_return co_await AsyncSendRequest(held, mode);
    }

    auto HttpClient::AsyncGet(
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        co_return co_await AsyncSendRequest(Request{ Verb::get, path, params, headers });
    }

    auto HttpClient::AsyncHead(
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        co_return co_await AsyncSendRequest(Request{ Verb::head, path, params, headers });
    }

    auto HttpClient::AsyncPost(
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        co_return co_await AsyncSendRequest(Request{ Verb::post, path, params, headers });
    }

    auto HttpClient::AsyncPut(
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        co_return co_await AsyncSendRequest(Request{ Verb::put, path, params, headers });
    }

    auto HttpClient::AsyncPatch(
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        co_return co_await AsyncSendRequest(Request{ Verb::patch, path, params, headers });
    }

    auto HttpClient::AsyncDelete(
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        co_return co_await AsyncSendRequest(Request{ Verb::delete_, path, params, headers });
    }

    auto HttpClient::AsyncOptions(
        std::string_view path,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        co_return co_await AsyncSendRequest(Request{ Verb::options, path, params, headers });
    }

    auto HttpClient::AsyncPost(
        std::string_view path,
        std::string_view body,
        std::string_view content_type,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        Request request{ Verb::post, path, params, headers };
        request.SetStringBody(std::string{ body }, content_type);
        co_return co_await AsyncSendRequest(request);
    }

    auto HttpClient::AsyncPost(
        std::string_view path,
        Json const& body,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        Request request{ Verb::post, path, params, headers };
        request.SetJsonBody(body);
        co_return co_await AsyncSendRequest(request);
    }

    auto HttpClient::AsyncPut(
        std::string_view path,
        std::string_view body,
        std::string_view content_type,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        Request request{ Verb::put, path, params, headers };
        request.SetStringBody(std::string{ body }, content_type);
        co_return co_await AsyncSendRequest(request);
    }

    auto HttpClient::AsyncPut(
        std::string_view path,
        Json const& body,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        Request request{ Verb::put, path, params, headers };
        request.SetJsonBody(body);
        co_return co_await AsyncSendRequest(request);
    }

    auto HttpClient::AsyncPatch(
        std::string_view path,
        std::string_view body,
        std::string_view content_type,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        Request request{ Verb::patch, path, params, headers };
        request.SetStringBody(std::string{ body }, content_type);
        co_return co_await AsyncSendRequest(request);
    }

    auto HttpClient::AsyncPatch(
        std::string_view path,
        Json const& body,
        QueryParams const& params,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        Request request{ Verb::patch, path, params, headers };
        request.SetJsonBody(body);
        co_return co_await AsyncSendRequest(request);
    }

    auto HttpClient::AsyncDownload(
        Verb method,
        std::string_view path,
        std::filesystem::path const& save_path,
        Headers const& headers
    ) -> net::awaitable<Result<Response>> {
        Request request{ method, path, headers };
        auto result{ co_await AsyncSendRequest(request, BodyMode::Lazy) };
        if (!result) {
            co_return result;
        }
        if (auto const ec{ co_await result->ReadToFile(save_path) }) {
            co_return std::unexpected{ ec };
        }
        co_return result;
    }

} // namespace mhttp::client
