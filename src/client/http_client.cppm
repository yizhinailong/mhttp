/**
 * @file http_client.cppm
 * @brief Expose an Asio-free single-flight buffered HTTP client on a borrowed Runtime.
 */
export module mhttp:client.http_client;

import std;
import :client.impl.client;
export import :core.runtime;
export import :client.options;
export import :client.request;
export import :client.response;

export namespace mhttp::client {

    /**
     * @brief Own one lazy HTTP connection with serialized single-flight Send operations.
     * @note Runtime must outlive the client and all issued Tasks. Destruction requests nonblocking
     *       cancellation; issued Tasks retain their own state until completion. Stop permits future
     *       requests to reconnect. Different clients can run concurrently on the same Runtime.
     */
    class HttpClient {
    private:
        std::shared_ptr<impl::ClientImpl> m_impl; ///< Owned connection/configuration independent of public lifetime.

    public:
        /**
         * @brief Own a normalized host/port pair without resolving or connecting.
         * @param runtime Longer-lived execution owner.
         * @param host Hostname or IPv6 literal, optionally bracketed; copied during construction.
         * @param port TCP port.
         * @param options Owned deadlines, limits, persistence policy, and fallback fields.
         */
        HttpClient(
            Runtime& runtime,
            std::string_view host,
            std::uint16_t port,
            ClientOptions options = {}
        ) : m_impl{ std::make_shared<impl::ClientImpl>(runtime, detail::client_origin(host, port), std::move(options)) } {}

        /**
         * @brief Own a validated HTTP URL, preserving its default path/query and excluding fragments on send.
         * @param runtime Longer-lived execution owner.
         * @param url Absolute URL, copied during construction; HTTPS reports UnsupportedScheme.
         * @param options Owned configuration snapshot.
         * @note InitializationError and every Send report malformed URL/configuration without network I/O.
         */
        HttpClient(Runtime& runtime, std::string_view url, ClientOptions options = {}) :
            m_impl{ std::make_shared<impl::ClientImpl>(runtime, Url::Parse(url), std::move(options)) } {}

        /**
         * @brief Keep connection identity and mutable single-flight state unique.
         * @param other Unused source.
         */
        HttpClient(HttpClient const& other) = delete;

        /**
         * @brief Prevent replacement of a client with issued operations.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(HttpClient const& other) -> HttpClient& = delete;

        /**
         * @brief Request cancellation without blocking an I/O thread or waiting for issued Tasks.
         */
        ~HttpClient() { Stop(); }

        /**
         * @brief Inspect immutable URL/configuration validation before issuing any I/O.
         * @return Constructor error, absent for a valid lazy client.
         */
        [[nodiscard]] auto InitializationError() const noexcept -> std::optional<Error> { return m_impl->InitializationError(); }

        /**
         * @brief Borrow the normalized constructor host while this object stays alive.
         * @return Bare hostname/address, empty for invalid construction.
         */
        [[nodiscard]] auto Host() const noexcept -> std::string_view { return m_impl->Host(); }

        /**
         * @brief Inspect the constructor destination port.
         * @return TCP port or zero for invalid construction.
         */
        [[nodiscard]] auto Port() const noexcept -> std::uint16_t { return m_impl->Port(); }

        /**
         * @brief Observe known connection state safely from any thread.
         * @return True after establishment until a known failure, closure, or Stop.
         * @note A peer may close an idle connection before the next operation observes it.
         */
        [[nodiscard]] auto IsOpen() const noexcept -> bool { return m_impl->IsOpen(); }

        /**
         * @brief Transfer one complete request into a lazy asynchronous exchange.
         * @param request Owned data; retained views must not be used after transfer.
         * @return Task yielding a complete owned response or recoverable error, including Busy for overlap.
         * @note HTTP error statuses are successful response values. The awaiting Task resumes on its
         *       original scheduler; unexpected allocation/executor exceptions may still propagate.
         */
        [[nodiscard]] auto Send(Request request) -> Task<Result<Response>> { return m_impl->Send(std::move(request)); }

        /**
         * @brief Cancel issued requests and request socket closure from any thread without blocking.
         * @note In-flight operations return Canceled. Requests issued after Stop may reconnect.
         */
        auto Stop() -> void { m_impl->Stop(); }
    };

} // namespace mhttp::client
