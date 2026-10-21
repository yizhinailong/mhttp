/**
 * @file http_client.cppm
 * @brief Expose an Asio-free single-flight bounded HTTP client on a borrowed Runtime.
 */
export module mhttp:client.http_client;

import std;
import :client.impl.client;
export import :core.runtime;
export import :client.options;
export import :client.request;
export import :client.response;
export import :client.transfer;

namespace mhttp::impl {
    class ClientPoolImpl;
} // namespace mhttp::impl

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
        std::weak_ptr<void> m_pool_lease; ///< Each issued operation independently retains its optional pool loan.
        friend class impl::ClientPoolImpl;

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
         * @brief Probe an established idle connection asynchronously without connecting or consuming bytes.
         * @return True for a quiet socket; false for no socket or observed closure; Busy during a transfer.
         * @note This best-effort probe waits up to one millisecond. The peer may close immediately afterward.
         *       Issuing a probe through a pooled handle retains its loan until the Task frame retires.
         */
        [[nodiscard]] auto IsAlive() -> Task<Result<bool>> { return m_impl->IsAlive(m_pool_lease.lock()); }

        /**
         * @brief Replace default fields for Tasks issued after this call, safely from any thread.
         * @param headers Owned fallback fields; an empty collection also clears configured authorization.
         * @return Success or InvalidHeaders for an invalid Host; failure preserves prior configuration.
         * @note Explicit request fields override defaults; issued Tasks retain their original snapshot.
         */
        [[nodiscard]] auto SetDefaultHeaders(Headers headers) -> Result<void> {
            return m_impl->SetDefaultHeaders(std::move(headers));
        }

        /**
         * @brief Set the Basic Authorization default for future Tasks from any thread.
         * @param username Borrowed identifier copied during this call; colons are rejected.
         * @param password Borrowed password copied into the encoded authorization value.
         * @return Success or InvalidRequest; explicit request Authorization still takes precedence.
         */
        [[nodiscard]] auto SetBasicAuth(
            std::string_view username,
            std::string_view password
        ) -> Result<void> {
            return m_impl->SetBasicAuth(username, password);
        }

        /**
         * @brief Set the Bearer Authorization default for future Tasks from any thread.
         * @param token Nonempty RFC 6750 token copied during this call; whitespace/control bytes are rejected.
         * @return Success or InvalidHeaders; failure preserves the previous authorization value.
         */
        [[nodiscard]] auto SetBearerToken(std::string_view token) -> Result<void> { return m_impl->SetBearerToken(token); }

        /**
         * @brief Transfer one complete request into a lazy asynchronous exchange.
         * @param request Owned data; retained views must not be used after transfer.
         * @return Task yielding a complete owned response or recoverable error, including Busy for overlap.
         * @note HTTP error statuses are successful response values. The awaiting Task resumes on its
         *       original scheduler; unexpected allocation/executor exceptions may still propagate.
         */
        [[nodiscard]] auto Send(Request request) -> Task<Result<Response>> {
            return m_impl->Send(std::move(request), {}, m_pool_lease.lock());
        }

        /**
         * @brief Send with owned asynchronous receivers and progress callbacks, applying bounded backpressure.
         * @param request Owned outgoing data transferred before initial suspension.
         * @param callbacks Owned callbacks; on_body streams chunks and leaves returned Body() empty.
         * @return Complete final metadata/trailers, or error after closing and draining an incomplete transfer.
         * @note Automatic redirects suppress intermediate headers/body/download callbacks. Upload samples
         *       identify each hop. All callbacks must finish before Send completes; Runtime outlives the Task.
         */
        [[nodiscard]] auto Send(
            Request request,
            TransferCallbacks callbacks
        ) -> Task<Result<Response>> {
            return m_impl->Send(std::move(request), std::move(callbacks), m_pool_lease.lock());
        }


        /**
         * @brief Send GET with an owned target, ordered query parameters, and explicit fields.
         * @param path Origin-form target; empty uses the constructor URL target.
         * @param query Additional parameters, copied or moved before initial suspension.
         * @param headers Explicit fields overriding configured defaults.
         * @return Lazy complete-response Task with the same error and ownership contract as Send.
         */
        [[nodiscard]] auto Get(
            std::string path = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Get, std::move(path), std::move(query), std::move(headers), {}, {});
        }

        /**
         * @brief Send GET with an owned binary body and an optional fallback media type.
         * @param path Origin-form target, or empty for the constructor URL target.
         * @param body Owned binary representation, including embedded NUL bytes.
         * @param content_type Optional Content-Type fallback; explicit headers take precedence.
         * @param query Additional ordered query parameters.
         * @param headers Explicit outgoing fields, preserving a supplied Content-Type even when empty.
         * @return Lazy complete-response Task; invalid used media types fail before network I/O.
         */
        [[nodiscard]] auto Get(
            std::string path,
            std::string body,
            std::string_view content_type = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Get, std::move(path), std::move(query), std::move(headers), std::move(body), content_type);
        }

        /**
         * @brief Send GET with optional JSON serialized before the lazy exchange is issued.
         * @tparam T Exact Json or OrderedJson type supplied by the optional JSON module.
         * @tparam Adapter Deferred implementation available after importing mhttp.json.
         * @param path Owned origin-form target, or empty for the constructor URL target.
         * @param value Borrowed JSON tree; no reference survives this synchronous call.
         * @param query Owned additional ordered parameters.
         * @param headers Explicit fields; a supplied Content-Type overrides application/json.
         * @return Lazy complete-response Task, or InvalidRequest for serialization failure without I/O.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto Get(
            std::string path,
            T const& value,
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendJsonRequest<T, Adapter>(Method::Get, std::move(path), value, std::move(query), std::move(headers));
        }

        /**
         * @brief Send HEAD with an owned target, ordered query parameters, and explicit fields.
         * @param path Origin-form target; empty uses the constructor URL target.
         * @param query Additional parameters, copied or moved before initial suspension.
         * @param headers Explicit fields overriding configured defaults.
         * @return Lazy complete-response Task with the same error and ownership contract as Send.
         */
        [[nodiscard]] auto Head(
            std::string path = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Head, std::move(path), std::move(query), std::move(headers), {}, {});
        }

        /**
         * @brief Send HEAD with an owned binary body and an optional fallback media type.
         * @param path Origin-form target, or empty for the constructor URL target.
         * @param body Owned binary representation, including embedded NUL bytes.
         * @param content_type Optional Content-Type fallback; explicit headers take precedence.
         * @param query Additional ordered query parameters.
         * @param headers Explicit outgoing fields, preserving a supplied Content-Type even when empty.
         * @return Lazy complete-response Task; invalid used media types fail before network I/O.
         */
        [[nodiscard]] auto Head(
            std::string path,
            std::string body,
            std::string_view content_type = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Head, std::move(path), std::move(query), std::move(headers), std::move(body), content_type);
        }

        /**
         * @brief Send HEAD with optional JSON serialized before the lazy exchange is issued.
         * @tparam T Exact Json or OrderedJson type supplied by the optional JSON module.
         * @tparam Adapter Deferred implementation available after importing mhttp.json.
         * @param path Owned origin-form target, or empty for the constructor URL target.
         * @param value Borrowed JSON tree; no reference survives this synchronous call.
         * @param query Owned additional ordered parameters.
         * @param headers Explicit fields; a supplied Content-Type overrides application/json.
         * @return Lazy complete-response Task, or InvalidRequest for serialization failure without I/O.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto Head(
            std::string path,
            T const& value,
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendJsonRequest<T, Adapter>(Method::Head, std::move(path), value, std::move(query), std::move(headers));
        }

        /**
         * @brief Send POST with an owned target, ordered query parameters, and explicit fields.
         * @param path Origin-form target; empty uses the constructor URL target.
         * @param query Additional parameters, copied or moved before initial suspension.
         * @param headers Explicit fields overriding configured defaults.
         * @return Lazy complete-response Task with the same error and ownership contract as Send.
         */
        [[nodiscard]] auto Post(
            std::string path = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Post, std::move(path), std::move(query), std::move(headers), {}, {});
        }

        /**
         * @brief Send POST with an owned binary body and an optional fallback media type.
         * @param path Origin-form target, or empty for the constructor URL target.
         * @param body Owned binary representation, including embedded NUL bytes.
         * @param content_type Optional Content-Type fallback; explicit headers take precedence.
         * @param query Additional ordered query parameters.
         * @param headers Explicit outgoing fields, preserving a supplied Content-Type even when empty.
         * @return Lazy complete-response Task; invalid used media types fail before network I/O.
         */
        [[nodiscard]] auto Post(
            std::string path,
            std::string body,
            std::string_view content_type = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Post, std::move(path), std::move(query), std::move(headers), std::move(body), content_type);
        }

        /**
         * @brief Send POST with optional JSON serialized before the lazy exchange is issued.
         * @tparam T Exact Json or OrderedJson type supplied by the optional JSON module.
         * @tparam Adapter Deferred implementation available after importing mhttp.json.
         * @param path Owned origin-form target, or empty for the constructor URL target.
         * @param value Borrowed JSON tree; no reference survives this synchronous call.
         * @param query Owned additional ordered parameters.
         * @param headers Explicit fields; a supplied Content-Type overrides application/json.
         * @return Lazy complete-response Task, or InvalidRequest for serialization failure without I/O.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto Post(
            std::string path,
            T const& value,
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendJsonRequest<T, Adapter>(Method::Post, std::move(path), value, std::move(query), std::move(headers));
        }

        /**
         * @brief Send PUT with an owned target, ordered query parameters, and explicit fields.
         * @param path Origin-form target; empty uses the constructor URL target.
         * @param query Additional parameters, copied or moved before initial suspension.
         * @param headers Explicit fields overriding configured defaults.
         * @return Lazy complete-response Task with the same error and ownership contract as Send.
         */
        [[nodiscard]] auto Put(
            std::string path = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Put, std::move(path), std::move(query), std::move(headers), {}, {});
        }

        /**
         * @brief Send PUT with an owned binary body and an optional fallback media type.
         * @param path Origin-form target, or empty for the constructor URL target.
         * @param body Owned binary representation, including embedded NUL bytes.
         * @param content_type Optional Content-Type fallback; explicit headers take precedence.
         * @param query Additional ordered query parameters.
         * @param headers Explicit outgoing fields, preserving a supplied Content-Type even when empty.
         * @return Lazy complete-response Task; invalid used media types fail before network I/O.
         */
        [[nodiscard]] auto Put(
            std::string path,
            std::string body,
            std::string_view content_type = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Put, std::move(path), std::move(query), std::move(headers), std::move(body), content_type);
        }

        /**
         * @brief Send PUT with optional JSON serialized before the lazy exchange is issued.
         * @tparam T Exact Json or OrderedJson type supplied by the optional JSON module.
         * @tparam Adapter Deferred implementation available after importing mhttp.json.
         * @param path Owned origin-form target, or empty for the constructor URL target.
         * @param value Borrowed JSON tree; no reference survives this synchronous call.
         * @param query Owned additional ordered parameters.
         * @param headers Explicit fields; a supplied Content-Type overrides application/json.
         * @return Lazy complete-response Task, or InvalidRequest for serialization failure without I/O.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto Put(
            std::string path,
            T const& value,
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendJsonRequest<T, Adapter>(Method::Put, std::move(path), value, std::move(query), std::move(headers));
        }

        /**
         * @brief Send PATCH with an owned target, ordered query parameters, and explicit fields.
         * @param path Origin-form target; empty uses the constructor URL target.
         * @param query Additional parameters, copied or moved before initial suspension.
         * @param headers Explicit fields overriding configured defaults.
         * @return Lazy complete-response Task with the same error and ownership contract as Send.
         */
        [[nodiscard]] auto Patch(
            std::string path = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Patch, std::move(path), std::move(query), std::move(headers), {}, {});
        }

        /**
         * @brief Send PATCH with an owned binary body and an optional fallback media type.
         * @param path Origin-form target, or empty for the constructor URL target.
         * @param body Owned binary representation, including embedded NUL bytes.
         * @param content_type Optional Content-Type fallback; explicit headers take precedence.
         * @param query Additional ordered query parameters.
         * @param headers Explicit outgoing fields, preserving a supplied Content-Type even when empty.
         * @return Lazy complete-response Task; invalid used media types fail before network I/O.
         */
        [[nodiscard]] auto Patch(
            std::string path,
            std::string body,
            std::string_view content_type = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Patch, std::move(path), std::move(query), std::move(headers), std::move(body), content_type);
        }

        /**
         * @brief Send PATCH with optional JSON serialized before the lazy exchange is issued.
         * @tparam T Exact Json or OrderedJson type supplied by the optional JSON module.
         * @tparam Adapter Deferred implementation available after importing mhttp.json.
         * @param path Owned origin-form target, or empty for the constructor URL target.
         * @param value Borrowed JSON tree; no reference survives this synchronous call.
         * @param query Owned additional ordered parameters.
         * @param headers Explicit fields; a supplied Content-Type overrides application/json.
         * @return Lazy complete-response Task, or InvalidRequest for serialization failure without I/O.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto Patch(
            std::string path,
            T const& value,
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendJsonRequest<T, Adapter>(Method::Patch, std::move(path), value, std::move(query), std::move(headers));
        }

        /**
         * @brief Send DELETE with an owned target, ordered query parameters, and explicit fields.
         * @param path Origin-form target; empty uses the constructor URL target.
         * @param query Additional parameters, copied or moved before initial suspension.
         * @param headers Explicit fields overriding configured defaults.
         * @return Lazy complete-response Task with the same error and ownership contract as Send.
         */
        [[nodiscard]] auto Delete(
            std::string path = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Delete, std::move(path), std::move(query), std::move(headers), {}, {});
        }

        /**
         * @brief Send DELETE with an owned binary body and an optional fallback media type.
         * @param path Origin-form target, or empty for the constructor URL target.
         * @param body Owned binary representation, including embedded NUL bytes.
         * @param content_type Optional Content-Type fallback; explicit headers take precedence.
         * @param query Additional ordered query parameters.
         * @param headers Explicit outgoing fields, preserving a supplied Content-Type even when empty.
         * @return Lazy complete-response Task; invalid used media types fail before network I/O.
         */
        [[nodiscard]] auto Delete(
            std::string path,
            std::string body,
            std::string_view content_type = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Delete, std::move(path), std::move(query), std::move(headers), std::move(body), content_type);
        }

        /**
         * @brief Send DELETE with optional JSON serialized before the lazy exchange is issued.
         * @tparam T Exact Json or OrderedJson type supplied by the optional JSON module.
         * @tparam Adapter Deferred implementation available after importing mhttp.json.
         * @param path Owned origin-form target, or empty for the constructor URL target.
         * @param value Borrowed JSON tree; no reference survives this synchronous call.
         * @param query Owned additional ordered parameters.
         * @param headers Explicit fields; a supplied Content-Type overrides application/json.
         * @return Lazy complete-response Task, or InvalidRequest for serialization failure without I/O.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto Delete(
            std::string path,
            T const& value,
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendJsonRequest<T, Adapter>(Method::Delete, std::move(path), value, std::move(query), std::move(headers));
        }

        /**
         * @brief Send OPTIONS with an owned target, ordered query parameters, and explicit fields.
         * @param path Origin-form target; empty uses the constructor URL target.
         * @param query Additional parameters, copied or moved before initial suspension.
         * @param headers Explicit fields overriding configured defaults.
         * @return Lazy complete-response Task with the same error and ownership contract as Send.
         */
        [[nodiscard]] auto Options(
            std::string path = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Options, std::move(path), std::move(query), std::move(headers), {}, {});
        }

        /**
         * @brief Send OPTIONS with an owned binary body and an optional fallback media type.
         * @param path Origin-form target, or empty for the constructor URL target.
         * @param body Owned binary representation, including embedded NUL bytes.
         * @param content_type Optional Content-Type fallback; explicit headers take precedence.
         * @param query Additional ordered query parameters.
         * @param headers Explicit outgoing fields, preserving a supplied Content-Type even when empty.
         * @return Lazy complete-response Task; invalid used media types fail before network I/O.
         */
        [[nodiscard]] auto Options(
            std::string path,
            std::string body,
            std::string_view content_type = {},
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendRequest(Method::Options, std::move(path), std::move(query), std::move(headers), std::move(body), content_type);
        }


        /**
         * @brief Send OPTIONS with optional JSON serialized before the lazy exchange is issued.
         * @tparam T Exact Json or OrderedJson type supplied by the optional JSON module.
         * @tparam Adapter Deferred implementation available after importing mhttp.json.
         * @param path Owned origin-form target, or empty for the constructor URL target.
         * @param value Borrowed JSON tree; no reference survives this synchronous call.
         * @param query Owned additional ordered parameters.
         * @param headers Explicit fields; a supplied Content-Type overrides application/json.
         * @return Lazy complete-response Task, or InvalidRequest for serialization failure without I/O.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto Options(
            std::string path,
            T const& value,
            QueryParams query = {},
            Headers headers = {}
        ) -> Task<Result<Response>> {
            return sendJsonRequest<T, Adapter>(Method::Options, std::move(path), value, std::move(query), std::move(headers));
        }

        /**
         * @brief Cancel issued requests and request socket closure from any thread without blocking.
         * @note In-flight operations return Canceled. Requests issued after Stop may reconnect.
         */
        auto Stop() -> void { m_impl->Stop(); }

    private:
        /**
         * @brief Return a shortcut validation failure without retaining a client or starting I/O.
         * @param error Recoverable request construction failure.
         * @return Owned lazy error result.
         */
        static auto failedRequest(Error error) -> Task<Result<Response>> { co_return std::unexpected{ error }; }

        /**
         * @brief Own serialized JSON synchronously and reuse ordinary shortcut request assembly.
         * @tparam T Supported exact JSON representation.
         * @tparam Adapter Selected serialization adapter.
         * @param method Outgoing method.
         * @param path Owned target.
         * @param value Borrowed tree, fully consumed before returning the Task.
         * @param query Owned query parameters.
         * @param headers Owned explicit fields overriding the JSON fallback media type.
         * @return Owned Send Task or serialization error without starting I/O.
         */
        template <typename T, typename Adapter>
            requires detail::JsonValue<T, Adapter>
        auto sendJsonRequest(
            Method method,
            std::string path,
            T const& value,
            QueryParams query,
            Headers headers
        ) -> Task<Result<Response>> {
            auto serialized = Adapter::Serialize(value);
            if (!serialized) { return failedRequest(serialized.error()); }
            return sendRequest(method, std::move(path), std::move(query), std::move(headers),
                std::move(*serialized), "application/json");
        }

        /**
         * @brief Assemble one owned convenience request synchronously before issuing its lazy Send.
         * @param method Known outgoing method.
         * @param path Owned raw target.
         * @param query Owned additional parameters.
         * @param headers Owned explicit fields.
         * @param body Optional owned representation.
         * @param content_type Borrowed fallback field, consumed synchronously before returning.
         * @return Owned Send or validation-failure Task.
         */
        auto sendRequest(
            Method method,
            std::string path,
            QueryParams query,
            Headers headers,
            std::optional<std::string> body,
            std::string_view content_type
        ) -> Task<Result<Response>> {
            Request request{ method, std::move(path) };
            request.Query() = std::move(query);
            if (body) {
                auto content = request.SetContent(std::move(*body));
                if (!content) { return failedRequest(content.error()); }
                if (!headers.Has("Content-Type") && !content_type.empty()) {
                    auto assigned = headers.Add("Content-Type", content_type);
                    if (!assigned) { return failedRequest(assigned.error()); }
                }
            }
            request.Headers() = std::move(headers);
            return Send(std::move(request));
        }
    };

} // namespace mhttp::client
