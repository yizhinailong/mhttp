/**
 * @file request.cppm
 * @brief Expose owned server request metadata, typed path captures, buffered or lazy content, and context.
 */
export module mhttp:server.request;

import std;
import :server.impl.request;
export import :core.message;
export import :core.query;
export import :core.endpoint;
export import :server.request_data;
export import :server.trusted_proxies;
export import :core.json_support;
export import :core.form_data;
export import :core.task;

export namespace mhttp::detail {

    struct ServerRequestAccess;

} // namespace mhttp::detail

export namespace mhttp::server {

    /**
     * @brief Own move-only request storage while exposing borrowed read-only HTTP metadata.
     * @note The session supplies a reference valid through the complete awaited handler chain.
     *       Do not move that borrowed object or retain its views beyond the request. Internal session
     *       initialization precedes handler access; RequestData alone supports concurrent mutation.
     *       A moved-from owned Request may only be destroyed or assigned.
     */
    class Request {
    private:
        std::shared_ptr<impl::ServerRequestImpl> m_impl; ///< Owned metadata, content, captures, and context.
        friend struct detail::ServerRequestAccess;

        /**
         * @brief Transfer nonnull initialized storage from the session's factory.
         * @param state Owned request storage.
         */
        explicit Request(std::shared_ptr<impl::ServerRequestImpl> state) : m_impl{ std::move(state) } {}

    public:
        /**
         * @brief Prevent accidental sharing of a request's mutable context and initialization state.
         * @param other Unused source.
         */
        Request(Request const& other) = delete;

        /**
         * @brief Prevent copied request aliases.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(Request const& other) -> Request& = delete;

        /**
         * @brief Transfer an owned request without relocating its borrowed storage.
         * @param other Previous owner, left suitable for destruction or assignment.
         */
        Request(Request&& other) noexcept = default;

        /**
         * @brief Replace owned request state with another request's state.
         * @param other Previous owner.
         * @return This request.
         */
        auto operator=(Request&& other) noexcept -> Request& = default;

        /**
         * @brief Recognize the case-sensitive method while preserving unknown extension tokens.
         * @return Known method, or nullopt for an unknown extension method.
         */
        [[nodiscard]] auto Method() const noexcept -> std::optional<mhttp::Method> {
            return ParseMethod(m_impl->head.method);
        }

        /**
         * @brief Borrow the exact case-sensitive method token, including extensions.
         * @return Token valid for this request's lifetime.
         */
        [[nodiscard]] auto MethodString() const noexcept -> std::string_view { return m_impl->head.method; }

        /**
         * @brief Borrow the encoded origin-form target, including its original query string.
         * @return Normalized parser target, or the literal OPTIONS asterisk.
         */
        [[nodiscard]] auto Target() const noexcept -> std::string_view { return m_impl->head.target; }

        /**
         * @brief Borrow the decoded path without its query string; + is a literal plus.
         * @return Application path view; encoded slashes have been decoded and are not routing separators.
         */
        [[nodiscard]] auto Path() const noexcept -> std::string_view { return m_impl->path; }

        /**
         * @brief Borrow ordered, decoded multi-value query parameters.
         * @return Request-owned collection; query + is decoded as space.
         */
        [[nodiscard]] auto Query() const noexcept -> QueryParams const& { return m_impl->query; }

        /**
         * @brief Borrow validated original request fields in wire order.
         * @return Read-only collection; trailer fields remain separate.
         */
        [[nodiscard]] auto Headers() const noexcept -> mhttp::Headers const& { return m_impl->head.headers; }

        /**
         * @brief Borrow the first case-insensitive request field value.
         * @param name Field name.
         * @param fallback Borrowed default if absent.
         * @return View into request storage or fallback.
         */
        [[nodiscard]] auto Header(
            std::string_view name,
            std::string_view fallback = {}
        ) const noexcept -> std::string_view { return m_impl->head.headers.Get(name, fallback); }

        /**
         * @brief Borrow buffered binary content after session body parsing.
         * @return Owned string, empty until body initialization completes.
         */
        [[nodiscard]] auto Body() const noexcept -> std::string const& { return m_impl->body; }

        /**
         * @brief Pull one owned nonempty decoded body chunk during a RouteLazy handler or its middleware/aspects.
         * @return Chunk up to request_chunk_bytes, nullopt at end, or an error; await every read sequentially.
         * @note Body() stays empty. Read through nullopt to permit persistence and publish Trailers().
         *       Framing that declares an empty body needs no read. Unread bytes force connection closure.
         *       Pre-routing, exception/error/post hooks, buffered requests, and deferred response providers
         *       cannot read. Issued Tasks own their reader but fail InvalidRequest after route retirement.
         *       read_timeout is one absolute intake deadline, enforced on reads without timing application work.
         *       The session preserves the first read failure for its final error hook, even when ignored.
         */
        [[nodiscard]] auto ReadBody() -> Task<Result<std::optional<std::string>>> { return m_impl->ReadBody(); }


        /**
         * @brief Decode application/x-www-form-urlencoded content on demand into independent owned parameters.
         * @return Ordered form fields, InvalidRequest for missing/other media types, or InvalidHeaders for duplicates.
         * @note Media types are case-insensitive and may have parameters. Decoding preserves binary bytes,
         *       repeated fields, and an initial ? in a field name. Each call returns fresh owned data;
         *       changing it does not affect Body or Query, and it may outlive this request.
         */
        [[nodiscard]] auto Form() const -> Result<QueryParams> { return m_impl->Form(); }

        /**
         * @brief Parse a complete multipart body on demand into independently owned fields and files.
         * @param limits Allocation/count/header bounds in addition to the server's HTTP request limit.
         * @return Owned form or recoverable syntax/header/size error; duplicate Content-Type is InvalidHeaders.
         * @note Parsing does not mutate Body or cache state; the result may outlive this request.
         */
        [[nodiscard]] auto Multipart(MultipartLimits limits = {}) const -> Result<FormData> {
            if (Headers().Count("Content-Type") > 1) { return std::unexpected{ Error::InvalidHeaders }; }
            return FormData::Parse(Body(), ContentType(), limits);
        }

        /**
         * @brief Parse an independent JSON value on demand when the optional JSON adapter is available.
         * @tparam Adapter Deferred adapter supplied by the json feature.
         * @param limits Positive nesting bound; HTTP body admission has already been applied.
         * @return Owned JSON or a recoverable media-type, syntax, header, or nesting-limit error.
         */
        template <typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonReader<Adapter>
        [[nodiscard]] auto Json(
            JsonLimits limits = {}
        ) const -> decltype(Adapter::Parse(Body(), Headers(), limits)) {
            return Adapter::Parse(Body(), Headers(), limits);
        }

        /**
         * @brief Borrow the complete Content-Type field value, including parameters.
         * @return Field value, or an empty view when absent.
         */
        [[nodiscard]] auto ContentType() const noexcept -> std::string_view { return Header("Content-Type"); }

        /**
         * @brief Borrow separately stored validated trailer fields after body parsing.
         * @return Read-only trailers, never merged into ordinary fields.
         */
        [[nodiscard]] auto Trailers() const noexcept -> mhttp::Headers const& { return m_impl->trailers; }

        /**
         * @brief Borrow a decoded named path capture, including a wildcard named *.
         * @param name Case-sensitive capture name.
         * @param fallback Borrowed default for an absent capture.
         * @return Borrowed decoded bytes; an existing capture may be empty or contain NUL.
         */
        [[nodiscard]] auto PathParam(
            std::string_view name,
            std::string_view fallback = {}
        ) const noexcept -> std::string_view { return m_impl->path_params.Get(name, fallback); }

        /**
         * @brief Convert a decoded capture strictly, or copy/borrow it as a string.
         * @tparam T Numeric type supported by QueryParams, bool, string, or string_view.
         * @param name Case-sensitive capture name.
         * @return Value or nullopt for absence, invalid numeric input, overflow, or a nonfinite number.
         * @note string_view borrows this request. string returns an independent copy and may allocate.
         */
        template <typename T>
            requires std::same_as<T, std::string> || std::same_as<T, std::string_view>
                || requires(QueryParams const& params, std::string_view key) { params.template Get<T>(key); }
        [[nodiscard]] auto PathParam(
            std::string_view name
        ) const noexcept(!std::same_as<T, std::string>) -> std::optional<T> {
            if constexpr (std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
                if (!m_impl->path_params.Has(name)) { return std::nullopt; }
                return T{ m_impl->path_params.Get(name) };
            } else { return m_impl->path_params.template Get<T>(name); }
        }

        /**
         * @brief Borrow the independent peer address and port value.
         * @return Endpoint valid throughout this request's lifetime.
         */
        [[nodiscard]] auto RemoteEndpoint() const noexcept -> Endpoint const& { return m_impl->remote; }

        /**
         * @brief Borrow the client IP computed from the server's immutable trusted proxy policy.
         * @return Owned address valid throughout this request's lifetime, including awaited handlers.
         * @note Defaults to RemoteEndpoint().address. Forwarded fields are considered only when the
         *       direct peer is trusted; the transport endpoint and its port always remain unchanged.
         */
        [[nodiscard]] auto ClientIp() const noexcept -> std::string const& { return m_impl->client_ip; }

        /**
         * @brief Borrow the independent local address and port value.
         * @return Endpoint valid throughout this request's lifetime.
         */
        [[nodiscard]] auto LocalEndpoint() const noexcept -> Endpoint const& { return m_impl->local; }

        /**
         * @brief Inspect the parsed wire version.
         * @return HTTP/1.0 or HTTP/1.1.
         */
        [[nodiscard]] auto Version() const noexcept -> mhttp::Version { return m_impl->head.version; }

        /**
         * @brief Inspect request persistence semantics, independent of server shutdown and response policy.
         * @return Whether request version and Connection tokens permit reuse.
         */
        [[nodiscard]] auto KeepAlive() const noexcept -> bool { return m_impl->keep_alive; }

        /**
         * @brief Borrow thread-safe context storage shared by this request's middleware and handler.
         * @return Mutable context, valid until the request is destroyed.
         */
        [[nodiscard]] auto Data() noexcept -> RequestData& { return m_impl->data; }

        /**
         * @brief Borrow context for nonconsuming copied lookup.
         * @return Read-only context, valid until the request is destroyed.
         */
        [[nodiscard]] auto Data() const noexcept -> RequestData const& { return m_impl->data; }
    };

} // namespace mhttp::server

export namespace mhttp::detail {

    /**
     * @brief Initialize a request using owned standard protocol types without exposing its implementation.
     * @internal
     */
    struct ServerRequestAccess {
        /**
         * @brief Create request storage independently of parser buffers and sockets.
         * @param head Validated and normalized parser metadata, consumed by value.
         * @param remote Owned peer endpoint.
         * @param local Owned local endpoint.
         * @param proxies Borrowed immutable policy, not retained after computing the client IP.
         * @return Move-only initialized request with an empty body and no captures or trailers yet.
         */
        [[nodiscard]] static auto Create(
            RequestHead head,
            Endpoint remote = {},
            Endpoint local = {},
            server::TrustedProxies const& proxies = {}
        ) -> server::Request {
            return server::Request{ std::make_shared<impl::ServerRequestImpl>(
                std::move(head), std::move(remote), std::move(local), proxies) };
        }

        /**
         * @brief Borrow encoded path bytes for splitting before decoding during route matching.
         * @param request Borrowed initialized request.
         * @return Encoded target prefix excluding its query string.
         */
        [[nodiscard]] static auto EncodedPath(
            server::Request const& request
        ) noexcept -> std::string_view {
            return request.m_impl->EncodedPath();
        }

        /**
         * @brief Attach owned lazy intake before application middleware, publishing trailers through a weak owner.
         * @param request Borrowed initialized request whose storage must not form a reader ownership cycle.
         * @param reader Owned state on the connection strand, retired by router/session cleanup.
         */
        static auto AttachBodyReader(
            server::Request& request,
            std::shared_ptr<impl::RequestBodyImpl> reader
        ) -> void {
            reader->OnComplete([weak = std::weak_ptr{ request.m_impl }](Headers trailers) {
                if (auto owner = weak.lock()) { owner->trailers = std::move(trailers); }
            });
            request.m_impl->reader = std::move(reader);
        }

        /**
         * @brief End lazy intake before final exception/error/post hooks and response commitment.
         * @param request Borrowed request, accessed only on its connection strand.
         */
        static auto RetireBody(server::Request& request) -> void {
            if (request.m_impl->reader) { request.m_impl->reader->Retire(); }
        }

        /**
         * @brief Inspect lazy body completion after the route chain drains.
         * @param request Borrowed request on the connection strand.
         * @return Completion, or nullopt for a buffered/standalone request.
         */
        [[nodiscard]] static auto BodyComplete(
            server::Request const& request
        ) -> std::optional<bool> {
            if (request.m_impl->reader) { return request.m_impl->reader->Complete(); }
            return std::nullopt;
        }

        /**
         * @brief Inspect a lazy body's sticky failure even if application code ignored it.
         * @param request Borrowed request on the connection strand.
         * @return First owned failure, or nullopt when no lazy read failed.
         */
        [[nodiscard]] static auto BodyFailure(
            server::Request const& request
        ) -> std::optional<ProtocolFailure> {
            if (request.m_impl->reader) { return request.m_impl->reader->Failure(); }
            return std::nullopt;
        }

        /**
         * @brief Transfer fully parsed buffered content before invoking application handlers.
         * @param request Request accessed serially by its session.
         * @param body Owned decoded content, already checked against the parser's body bound.
         */
        static auto SetBody(server::Request& request, std::string body) -> void {
            request.m_impl->body = std::move(body);
        }

        /**
         * @brief Transfer matched decoded captures before executing route middleware or handlers.
         * @param request Request accessed serially by its session.
         * @param params Owned captures from the selected route.
         */
        static auto SetPathParams(server::Request& request, QueryParams params) -> void {
            request.m_impl->path_params = std::move(params);
        }

        /**
         * @brief Transfer filtered trailer fields after complete body parsing.
         * @param request Request accessed serially by its session.
         * @param trailers Validated and filtered protocol trailer collection.
         */
        static auto SetTrailers(server::Request& request, Headers trailers) -> void {
            request.m_impl->trailers = std::move(trailers);
        }
    };

} // namespace mhttp::detail
