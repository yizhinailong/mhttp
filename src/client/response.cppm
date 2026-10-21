/**
 * @file response.cppm
 * @brief Expose read-only owned client response data independently of transport and parser lifetimes.
 */
export module mhttp:client.response;

import std;
import :client.impl.response;
export import :core.message;
export import :core.endpoint;

export namespace mhttp::detail {

    struct ClientResponseAccess;

} // namespace mhttp::detail

export namespace mhttp::client {

    /**
     * @brief Own move-only immutable response data that can outlive its originating client.
     * @note Returned views and references borrow this response until assignment or destruction.
     *       Concurrent read-only access is safe while ownership stays alive. A moved-from response
     *       may only be destroyed or assigned. HTTP error statuses are ordinary response values.
     */
    class Response {
    private:
        std::shared_ptr<impl::ClientResponseImpl const> m_impl; ///< Owned immutable parsed response storage.
        friend struct detail::ClientResponseAccess;

        /**
         * @brief Retain fully initialized immutable state from the parser boundary.
         * @param state Owned complete response data.
         */
        explicit Response(std::shared_ptr<impl::ClientResponseImpl const> state) : m_impl{ std::move(state) } {}

    public:
        /**
         * @brief Create an empty unparsed response with Unknown status and no reuse permission.
         */
        Response() : m_impl{ std::make_shared<impl::ClientResponseImpl>() } {}

        /**
         * @brief Keep response ownership explicit instead of creating aliases.
         * @param other Unused source.
         */
        Response(Response const& other) = delete;

        /**
         * @brief Prevent copied aliases to response-owned borrowed data.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(Response const& other) -> Response& = delete;

        /**
         * @brief Transfer immutable response ownership without relocating its fields or body.
         * @param other Previous owner, left suitable for destruction or assignment.
         */
        Response(Response&& other) noexcept = default;

        /**
         * @brief Release current response storage and take another response's data.
         * @param other Previous owner.
         * @return This response.
         */
        auto operator=(Response&& other) noexcept -> Response& = default;

        /**
         * @brief Inspect the received HTTP status, including extension codes.
         * @return Parsed status, or Unknown for a default unparsed response.
         */
        [[nodiscard]] auto Status() const noexcept -> mhttp::Status { return m_impl->head.status; }

        /**
         * @brief Borrow the exact received reason phrase, which may be empty.
         * @return View owned by this response.
         */
        [[nodiscard]] auto Reason() const noexcept -> std::string_view { return m_impl->head.reason; }

        /**
         * @brief Inspect the parsed response protocol version.
         * @return Supported HTTP version.
         */
        [[nodiscard]] auto Version() const noexcept -> mhttp::Version { return m_impl->head.version; }

        /**
         * @brief Borrow validated response fields in wire order.
         * @return Immutable fields, preserving repeated values and original spelling.
         */
        [[nodiscard]] auto Headers() const noexcept -> mhttp::Headers const& { return m_impl->head.headers; }

        /**
         * @brief Borrow the first case-insensitive response field.
         * @param name Field name.
         * @param fallback Borrowed default for an absent field.
         * @return Borrowed field value or fallback.
         */
        [[nodiscard]] auto Header(
            std::string_view name,
            std::string_view fallback = {}
        ) const noexcept -> std::string_view { return m_impl->head.headers.Get(name, fallback); }

        /**
         * @brief Borrow the complete media type field, including parameters.
         * @return Content-Type or an empty view.
         */
        [[nodiscard]] auto ContentType() const noexcept -> std::string_view { return Header("Content-Type"); }

        /**
         * @brief Borrow the complete binary response body after decoding transfer framing.
         * @return Owned bytes, empty for bodyless responses.
         */
        [[nodiscard]] auto Body() const noexcept -> std::string const& { return m_impl->body; }

        /**
         * @brief Borrow trailers separately from ordinary response headers.
         * @return Immutable validated trailer fields.
         */
        [[nodiscard]] auto Trailers() const noexcept -> mhttp::Headers const& { return m_impl->trailers; }

        /**
         * @brief Borrow the peer endpoint snapshot taken for this response.
         * @return Owned endpoint, empty when no network snapshot was supplied.
         */
        [[nodiscard]] auto RemoteEndpoint() const noexcept -> Endpoint const& { return m_impl->remote; }

        /**
         * @brief Borrow the local endpoint snapshot taken for this response.
         * @return Owned endpoint, empty when no network snapshot was supplied.
         */
        [[nodiscard]] auto LocalEndpoint() const noexcept -> Endpoint const& { return m_impl->local; }

        /**
         * @brief Inspect whether complete response framing allowed the connection to remain reusable.
         * @return False for closing/EOF-delimited responses; a later peer close can still invalidate reuse.
         */
        [[nodiscard]] auto KeepAlive() const noexcept -> bool { return m_impl->keep_alive; }
    };

} // namespace mhttp::client

export namespace mhttp::detail {

    /**
     * @brief Initialize immutable client response state at the completed parser boundary.
     * @internal
     */
    struct ClientResponseAccess {
        /**
         * @brief Transfer complete parsed data into response-owned immutable storage.
         * @param head Owned parsed response metadata.
         * @param body Owned decoded binary body.
         * @param trailers Owned validated trailers.
         * @param keep_alive Final response connection policy.
         * @param remote Owned peer snapshot.
         * @param local Owned local snapshot.
         * @return Response independent of all supplied values and the parser's lifetime.
         */
        [[nodiscard]] static auto Create(
            ResponseHead head,
            std::string body,
            Headers trailers = {},
            bool keep_alive = false,
            Endpoint remote = {},
            Endpoint local = {}
        ) -> client::Response {
            auto state = std::make_shared<impl::ClientResponseImpl>();
            state->head = std::move(head);
            state->body = std::move(body);
            state->trailers = std::move(trailers);
            state->remote = std::move(remote);
            state->local = std::move(local);
            state->keep_alive = keep_alive;
            return client::Response{ std::move(state) };
        }
    };

} // namespace mhttp::detail
