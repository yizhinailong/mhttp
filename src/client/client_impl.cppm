/**
 * @file client_impl.cppm
 * @brief Own one client connection: connect on demand, timeouts, single-flight bookkeeping.
 */
module;

#include <spdlog/spdlog.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:client_impl;

import std;
import :config;
import :error;
import :logging;
import :url;
import :connection;
import :http_stream;

export namespace mhttp::client {

    /**
     * @brief How request timeouts are measured.
     */
    enum class TimeoutPolicy {
        Overall, ///< One deadline for connect, send, and receive of a request.
        Step,    ///< Every I/O step gets the full timeout.
        Never,   ///< No timeout.
    };

} // namespace mhttp::client

namespace mhttp::detail {

    /**
     * @brief State behind one HttpClient: a lazily connected stream and its I/O primitives.
     *
     * Implements ClientConnection for request writers and response readers. Every socket
     * operation is spawned onto the client strand and uses a snapshot of the stream, so a
     * concurrent close only makes in-flight operations fail. Configuration setters are atomic and
     * callable from any thread.
     *
     * Single flight: TryBeginExchange() admits one request at a time. The exchange lasts until
     * the response header arrives; an unread response body then keeps the client busy until it
     * is read to the end or the response is destroyed. httplib did not guard eager requests.
     */
    class ClientImpl : public ClientConnection, public std::enable_shared_from_this<ClientImpl> {
    public:
        using HeaderParser = http::response_parser<http::empty_body>;

    private:
        net::any_io_executor m_executor;                                                          ///< Executor the strand runs on.
        net::strand<net::any_io_executor> m_strand;                                               ///< Serializes all connection I/O.
        tcp::resolver m_resolver;                                                                 ///< Name resolution; strand only.
        std::string const m_host;                                                                 ///< Host to connect to.
        std::string const m_host_value;                                                           ///< Host header value.
        std::uint16_t const m_port;                                                               ///< Port to connect to.
        url::Scheme const m_scheme;                                                               ///< Transport.
        LoggerHolder m_logger{ "mhttp.client" };                                                  ///< Replaceable logger.
        std::atomic<client::TimeoutPolicy> m_timeout_policy{ client::TimeoutPolicy::Overall };    ///< Timeout measurement.
        std::atomic<std::chrono::steady_clock::duration> m_timeout{ std::chrono::seconds{ 30 } }; ///< Timeout length.
        std::atomic<int> m_max_redirects{ 0 };                                                    ///< Redirects to follow; 0 = none.
        std::atomic<std::uint32_t> m_header_limit{ 64 * 1024 };                                   ///< Maximum response header bytes.
        std::atomic<std::uint64_t> m_body_limit{ 1024ULL * 1024 * 1024 };                         ///< Maximum response body bytes.
        std::atomic<std::uint64_t> m_download_rate_limit{ 0 };                                    ///< Read cap in bytes/s; 0 = none.
        std::atomic<std::uint64_t> m_upload_rate_limit{ 0 };                                      ///< Write cap in bytes/s; 0 = none.
        std::atomic<std::shared_ptr<HttpStream>> m_stream;                                        ///< Current connection; null when closed.
        beast::flat_buffer m_buffer;                                                              ///< Read buffer; strand only.
        bool m_deadline_active{ false };                                                          ///< Overall deadline armed; strand only.
        std::size_t m_responses_on_connection{ 0 };                                               ///< Responses read since connect; strand only.
        bool m_attempt_reused{ false };                                                           ///< Last message went over a used connection; strand only.
        mutable std::mutex m_mutex;                                                               ///< Guards the two flags below.
        bool m_exchange_active{ false };                                                          ///< A request awaits its response header.
        bool m_body_pending{ false };                                                             ///< A response body is not read yet.
        std::atomic<std::size_t> m_live_writers{ 0 };                                             ///< Alive LazyRequest objects.

    public:
        /**
         * @brief Create a closed client connection.
         * @param executor Executor for the client's own strand.
         * @param host Host name or address.
         * @param port Port.
         * @param scheme Transport; TLS fails at connect time until it is ported.
         */
        ClientImpl(
            net::any_io_executor executor,
            std::string_view host,
            std::uint16_t port,
            url::Scheme scheme
        ) : m_executor{ executor }, m_strand{ net::make_strand(executor) }, m_resolver{ m_strand }, m_host{ host }, m_host_value{ url::make_host_value(host, port, scheme) }, m_port{ port }, m_scheme{ scheme } {}

        ClientImpl(ClientImpl const&) = delete;
        auto operator=(ClientImpl const&) -> ClientImpl& = delete;

        // ---- configuration (any thread) ----

        [[nodiscard]] auto Host() const -> std::string_view {
            return m_host;
        }

        [[nodiscard]] auto HostValue() const -> std::string_view {
            return m_host_value;
        }

        [[nodiscard]] auto Port() const -> std::uint16_t {
            return m_port;
        }

        [[nodiscard]] auto GetScheme() const -> url::Scheme {
            return m_scheme;
        }

        /**
         * @brief Return the executor the client strand was made from.
         * @return Underlying executor; a redirect to another origin builds its client on it.
         */
        [[nodiscard]] auto BaseExecutor() const -> net::any_io_executor {
            return m_executor;
        }

        [[nodiscard]] auto Logger() const -> LoggerHolder const& {
            return m_logger;
        }

        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
            m_logger.SetLogger(std::move(logger));
        }

        auto SetTimeoutPolicy(client::TimeoutPolicy policy) -> void {
            m_timeout_policy.store(policy);
        }

        auto SetTimeout(std::chrono::steady_clock::duration timeout) -> void {
            m_timeout.store(timeout);
        }

        auto SetMaxRedirects(int count) -> void {
            m_max_redirects.store(count);
        }

        [[nodiscard]] auto MaxRedirects() const -> int {
            return m_max_redirects.load();
        }

        auto SetHeaderLimit(std::uint32_t limit) -> void {
            m_header_limit.store(limit);
        }

        auto SetBodyLimit(std::uint64_t limit) -> void {
            m_body_limit.store(limit);
        }

        /**
         * @brief Cap download and upload throughput; applied to the live stream on its strand.
         * @param download Read cap in bytes/s; 0 means unlimited.
         * @param upload Write cap in bytes/s; 0 means unlimited.
         */
        auto SetRateLimits(std::uint64_t download, std::uint64_t upload) -> void {
            m_download_rate_limit.store(download);
            m_upload_rate_limit.store(upload);
            net::dispatch(m_strand, [self = shared_from_this()]() {
                if (auto const stream{ self->m_stream.load() }) {
                    self->applyRateLimits(*stream);
                }
            });
        }

        [[nodiscard]] auto DownloadRateLimit() const -> std::uint64_t {
            return m_download_rate_limit.load();
        }

        [[nodiscard]] auto UploadRateLimit() const -> std::uint64_t {
            return m_upload_rate_limit.load();
        }

        /**
         * @brief Copy every per-connection setting from another client (used by redirects).
         * @param other Client to copy from.
         */
        auto CopySettingsFrom(ClientImpl const& other) -> void {
            m_timeout_policy.store(other.m_timeout_policy.load());
            m_timeout.store(other.m_timeout.load());
            m_header_limit.store(other.m_header_limit.load());
            m_body_limit.store(other.m_body_limit.load());
            m_download_rate_limit.store(other.m_download_rate_limit.load());
            m_upload_rate_limit.store(other.m_upload_rate_limit.load());
            m_logger.SetLogger(other.m_logger.GetLogger());
        }

        // ---- connection state ----

        /**
         * @brief Report whether a connection is open.
         * @return Snapshot; reliable only while the client is idle.
         */
        [[nodiscard]] auto IsOpen() const -> bool {
            auto const stream{ m_stream.load() };
            return stream && stream->IsOpen();
        }

        /**
         * @brief Report whether the client is busy: a live LazyRequest, a request in flight, or
         *        an unread response body.
         * @return False when the connection may serve another request or return to a pool.
         */
        [[nodiscard]] auto HasActiveSession() const -> bool {
            if (m_live_writers.load() != 0) {
                return true;
            }
            std::lock_guard lock{ m_mutex };
            return m_exchange_active || m_body_pending;
        }

        /**
         * @brief Probe whether the idle connection is still usable.
         * @return False when closed by the peer or holding stray bytes.
         * @note Runs on the client strand via co_spawn, so the caller resumes on its own
         *       executor. httplib dispatched the calling coroutine onto the client strand, and a
         *       pool validating a connection then kept touching its own state from there.
         */
        auto AsyncIsAlive() -> net::awaitable<bool> {
            co_return co_await net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<bool> {
                    auto const stream{ self->m_stream.load() };
                    co_return stream && stream->IsOpen() && stream->IsPeerAlive();
                },
                net::use_awaitable
            );
        }

        /**
         * @brief Close the connection on the strand; the caller resumes on its own executor.
         */
        auto AsyncClose() -> net::awaitable<void> {
            co_await net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<void> {
                    self->closeNow();
                    co_return;
                },
                net::use_awaitable
            );
        }

        /**
         * @brief Close the connection from any thread.
         * @return Future ready once closed; do not wait on it from the client's own I/O thread.
         */
        auto Close() -> std::future<void> {
            return net::co_spawn(
                m_strand,
                [self = shared_from_this()]() -> net::awaitable<void> {
                    self->closeNow();
                    co_return;
                },
                net::use_future
            );
        }

        auto AddWriter() -> void {
            ++m_live_writers;
        }

        auto RemoveWriter() -> void {
            --m_live_writers;
        }

        // ---- single-flight bookkeeping (any thread) ----

        /**
         * @brief Admit a new request.
         * @return Error::AlreadyInUse while another request or an unread body is pending.
         */
        auto TryBeginExchange() -> boost::system::error_code {
            std::lock_guard lock{ m_mutex };
            if (m_exchange_active || m_body_pending) {
                return make_error_code(Error::AlreadyInUse);
            }
            m_exchange_active = true;
            return {};
        }

        /**
         * @brief End the request phase once the response header arrived.
         * @param body_pending Whether the response body still has to be read.
         */
        auto CompleteExchange(bool body_pending) -> void {
            std::lock_guard lock{ m_mutex };
            m_exchange_active = false;
            m_body_pending = body_pending;
        }

        /**
         * @brief End a request that will get no response object; closes the connection.
         */
        auto AbortExchange() -> void {
            {
                std::lock_guard lock{ m_mutex };
                m_exchange_active = false;
            }
            closeFromAnyThread();
        }

        /**
         * @brief Record that the response body was read to the end.
         */
        auto EndBody() -> void {
            std::lock_guard lock{ m_mutex };
            m_body_pending = false;
        }

        /**
         * @brief Give up an unread response body; closes the connection so no bytes are left over.
         */
        auto AbandonBody() -> void {
            {
                std::lock_guard lock{ m_mutex };
                m_body_pending = false;
            }
            closeFromAnyThread();
        }

        /**
         * @brief Report whether the message being sent used a connection that served a response.
         * @return True when a failure may just mean the server closed an idle connection.
         * @note Read on the strand, after WriteHeader().
         */
        [[nodiscard]] auto AttemptReused() const -> bool {
            return m_attempt_reused;
        }

        // ---- I/O ----

        /**
         * @brief Read the next final response header, skipping 1xx interim responses.
         * @param method Request method; HEAD responses have no body.
         * @return Parser holding the header, or the read error (the connection is then closed).
         */
        auto ReadResponseHeader(
            Verb method
        ) -> net::awaitable<Result<std::unique_ptr<HeaderParser>>> {
            co_return co_await net::co_spawn(
                m_strand,
                [this, self = shared_from_this(), method]() -> net::awaitable<Result<std::unique_ptr<HeaderParser>>> {
                    for (;;) {
                        auto parser{ std::make_unique<HeaderParser>() };
                        parser->skip(method == Verb::head);
                        parser->header_limit(m_header_limit.load());
                        parser->body_limit(m_body_limit.load());

                        auto const stream{ m_stream.load() };
                        if (!stream) {
                            co_return std::unexpected{ make_error_code(net::error::not_connected) };
                        }
                        boost::system::error_code ec;
                        beginIo(*stream);
                        co_await http::async_read_header(*stream, m_buffer, *parser, net::redirect_error(net::use_awaitable, ec));
                        endIo(*stream);
                        if (ec) {
                            closeNow();
                            co_return std::unexpected{ ec };
                        }
                        // httplib returned interim responses (such as 100 Continue) as the result.
                        auto const code{ parser->get().result_int() };
                        if (code / 100 == 1 && code != 101) {
                            continue;
                        }
                        ++m_responses_on_connection;
                        finishIfDone(*parser);
                        co_return parser;
                    }
                },
                net::use_awaitable
            );
        }

        auto Executor() -> net::any_io_executor override {
            return m_strand;
        }

        auto ReadSome(
            Parser& parser,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await net::co_spawn(
                m_strand,
                [&, self = shared_from_this()]() -> net::awaitable<void> {
                    auto const stream{ m_stream.load() };
                    if (!stream) {
                        ec = net::error::not_connected;
                        co_return;
                    }
                    beginIo(*stream);
                    co_await http::async_read_some(*stream, m_buffer, parser, net::redirect_error(net::use_awaitable, ec));
                    endIo(*stream);
                    if (ec && ec != http::error::need_buffer) {
                        closeNow();
                        co_return;
                    }
                    finishIfDone(parser);
                },
                net::use_awaitable
            );
        }

        /**
         * @brief Write a request header, connecting first when no connection is open.
         *
         * Starting a new message re-arms the overall deadline and clears the read buffer, so
         * bytes left by an earlier response cannot be taken for the next one.
         */
        auto WriteHeader(
            Serializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await net::co_spawn(
                m_strand,
                [&, self = shared_from_this()]() -> net::awaitable<void> {
                    finishIo();
                    m_attempt_reused = isOpenNow() && m_responses_on_connection > 0;
                    if (!isOpenNow()) {
                        co_await connect(ec);
                        if (ec) {
                            co_return;
                        }
                    }
                    m_buffer.clear();
                    auto const stream{ m_stream.load() };
                    beginIo(*stream);
                    co_await http::async_write_header(*stream, serializer, net::redirect_error(net::use_awaitable, ec));
                    endIo(*stream);
                    if (ec) {
                        closeNow();
                    }
                },
                net::use_awaitable
            );
        }

        auto Write(
            Serializer& serializer,
            boost::system::error_code& ec
        ) -> net::awaitable<void> override {
            co_await net::co_spawn(
                m_strand,
                [&, self = shared_from_this()]() -> net::awaitable<void> {
                    auto const stream{ m_stream.load() };
                    if (!stream) {
                        ec = net::error::not_connected;
                        co_return;
                    }
                    beginIo(*stream);
                    co_await http::async_write(*stream, serializer, net::redirect_error(net::use_awaitable, ec));
                    endIo(*stream);
                    if (ec && ec != http::error::need_buffer) {
                        closeNow();
                    }
                },
                net::use_awaitable
            );
        }

    private:
        /**
         * @brief Report whether the connection is open; strand only.
         * @return True when a stream exists and its socket is open.
         */
        [[nodiscard]] auto isOpenNow() const -> bool {
            auto const stream{ m_stream.load() };
            return stream && stream->IsOpen();
        }

        /**
         * @brief Open a new connection: resolve, connect, and apply settings; strand only.
         * @param ec Receives the resolve or connect error.
         */
        auto connect(boost::system::error_code& ec) -> net::awaitable<void> {
            closeNow();
            if (m_scheme == url::Scheme::Tls) {
                // TLS is not ported yet; fail before any byte reaches a TLS port.
                ec = net::error::operation_not_supported;
                co_return;
            }
            auto const stream{ std::make_shared<HttpStream>(HttpStream::PlainStream{ m_strand }) };
            m_stream.store(stream);
            m_responses_on_connection = 0;
            applyRateLimits(*stream);

            beginIo(*stream);
            boost::system::error_code address_ec;
            auto const address{ net::ip::make_address(m_host, address_ec) };
            if (!address_ec) {
                co_await stream->AsyncConnect(tcp::endpoint{ address, m_port }, ec);
            } else {
                auto const endpoints{ co_await m_resolver.async_resolve(m_host, std::to_string(m_port), net::redirect_error(net::use_awaitable, ec)) };
                if (!ec) {
                    co_await stream->AsyncConnect(endpoints, ec);
                }
            }
            endIo(*stream);
            if (ec) {
                m_logger.GetLogger()->warn("connect {} failed: {}", url::make_url_value(m_host, m_port, m_scheme), ec.message());
                closeNow();
            }
        }

        /**
         * @brief Close the stream and cancel name resolution; strand only.
         */
        auto closeNow() -> void {
            m_resolver.cancel();
            if (auto const stream{ m_stream.exchange(nullptr) }) {
                boost::system::error_code ec;
                stream->Close(ec);
            }
            m_deadline_active = false;
        }

        /**
         * @brief Close on the strand, inline when already running there.
         */
        auto closeFromAnyThread() -> void {
            net::dispatch(m_strand, [self = shared_from_this()]() { self->closeNow(); });
        }

        /**
         * @brief Finish the response once its parser is done; close if it was not keep-alive.
         * @tparam ParserType Header or body parser.
         * @param parser Parser after a read.
         */
        template <typename ParserType>
        auto finishIfDone(ParserType const& parser) -> void {
            if (!parser.is_done()) {
                return;
            }
            finishIo();
            if (!parser.keep_alive()) {
                closeNow();
            }
        }

        /**
         * @brief Arm the timeout before an I/O step according to the policy.
         * @param stream Stream about to be used.
         */
        auto beginIo(HttpStream& stream) -> void {
            switch (m_timeout_policy.load()) {
            case client::TimeoutPolicy::Overall:
                if (!m_deadline_active) {
                    stream.ExpiresAfter(m_timeout.load());
                    m_deadline_active = true;
                }
                break;
            case client::TimeoutPolicy::Step:
                stream.ExpiresAfter(m_timeout.load());
                break;
            case client::TimeoutPolicy::Never:
                stream.ExpiresNever();
                break;
            }
        }

        /**
         * @brief Disarm a per-step timeout after an I/O step; the overall deadline keeps running.
         * @param stream Stream that was used.
         */
        auto endIo(HttpStream& stream) -> void {
            if (m_timeout_policy.load() != client::TimeoutPolicy::Overall) {
                stream.ExpiresNever();
            }
        }

        /**
         * @brief End the overall deadline of a request.
         */
        auto finishIo() -> void {
            m_deadline_active = false;
            if (auto const stream{ m_stream.load() }) {
                stream->ExpiresNever();
            }
        }

        /**
         * @brief Apply the stored rate limits to a stream; strand only.
         * @param stream Stream to configure.
         */
        auto applyRateLimits(HttpStream& stream) const -> void {
            stream.SetRateLimits(m_download_rate_limit.load(), m_upload_rate_limit.load());
        }
    };

    /**
     * @brief Hold one admitted request; aborts it (closing the connection) unless completed.
     */
    class ExchangeGuard {
    private:
        std::shared_ptr<ClientImpl> m_client; ///< Client the request runs on; null once done.

    public:
        ExchangeGuard() = default;

        ExchangeGuard(ExchangeGuard&& other) noexcept : m_client{ std::move(other.m_client) } {}

        auto operator=(ExchangeGuard&& other) noexcept -> ExchangeGuard& {
            if (this != &other) {
                abort();
                m_client = std::move(other.m_client);
            }
            return *this;
        }

        ExchangeGuard(ExchangeGuard const&) = delete;
        auto operator=(ExchangeGuard const&) -> ExchangeGuard& = delete;

        ~ExchangeGuard() {
            abort();
        }

        /**
         * @brief Admit a request on a client.
         * @param client Client to use.
         * @return Error::AlreadyInUse when the client is busy.
         */
        auto Begin(std::shared_ptr<ClientImpl> client) -> boost::system::error_code {
            abort();
            if (auto const ec{ client->TryBeginExchange() }) {
                return ec;
            }
            m_client = std::move(client);
            return {};
        }

        /**
         * @brief Report whether a request is admitted and not yet completed.
         * @return True between Begin() and Complete().
         */
        [[nodiscard]] auto Active() const noexcept -> bool {
            return m_client != nullptr;
        }

        /**
         * @brief Mark the response header as received.
         * @param body_pending Whether the response body still has to be read.
         */
        auto Complete(bool body_pending) -> void {
            if (auto const client{ std::exchange(m_client, nullptr) }) {
                client->CompleteExchange(body_pending);
            }
        }

    private:
        auto abort() -> void {
            if (auto const client{ std::exchange(m_client, nullptr) }) {
                client->AbortExchange();
            }
        }
    };

} // namespace mhttp::detail
