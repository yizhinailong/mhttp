/**
 * @file response.cppm
 * @brief Expose buffered server response content and validated headers without transport types.
 */
export module mhttp:server.response;

import std;
import :server.impl.response;
export import :core.message;
export import :core.json_support;
export import :server.stream_writer;

export namespace mhttp::detail {

    struct ServerResponseAccess;

} // namespace mhttp::detail

export namespace mhttp::server {

    /**
     * @brief Own a move-only buffered or streamed response for one serialized request handler chain.
     * @note Views and references borrow this response's state and expire on relevant mutation or
     *       destruction. Concurrent mutation requires external synchronization. A moved-from response
     *       may only be destroyed or assigned. Wire framing follows Body or the provider's declared length.
     *       Do not move a session-supplied borrowed response or retain it beyond its handler chain.
     */
    class Response {
    private:
        std::shared_ptr<impl::ServerResponseImpl> m_impl; ///< Owned representation and validated fields.
        friend struct detail::ServerResponseAccess;

    public:
        /**
         * @brief Create an empty 200 OK response with no inferred content type.
         */
        Response() : m_impl{ std::make_shared<impl::ServerResponseImpl>() } {}

        /**
         * @brief Prevent accidental aliasing of mutable response state.
         * @param other Unused source.
         */
        Response(Response const& other) = delete;

        /**
         * @brief Prevent replacement through a copied mutable alias.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(Response const& other) -> Response& = delete;

        /**
         * @brief Transfer response ownership without moving its borrowed representation storage.
         * @param other Previous owner, left suitable for destruction or assignment.
         */
        Response(Response&& other) noexcept = default;

        /**
         * @brief Release current state and transfer another response's ownership.
         * @param other Previous owner.
         * @return This response.
         */
        auto operator=(Response&& other) noexcept -> Response& = default;

        /**
         * @brief Inspect the current response status.
         * @return Known or extension three-digit status.
         */
        [[nodiscard]] auto Status() const noexcept -> mhttp::Status { return m_impl->status; }

        /**
         * @brief Set a status without changing representation bytes or fields.
         * @param status Known or extension status in [100, 999].
         * @return Success or InvalidRequest, preserving the previous status on rejection.
         */
        [[nodiscard]] auto SetStatus(mhttp::Status status) -> Result<void> { return m_impl->SetStatus(status); }

        /**
         * @brief Borrow validated mutable response fields.
         * @return Collection whose Set and Add operations reject header injection.
         */
        [[nodiscard]] auto Headers() noexcept -> mhttp::Headers& { return m_impl->headers; }

        /**
         * @brief Borrow the ordered response fields without mutation.
         * @return Collection valid while this response's state lives.
         */
        [[nodiscard]] auto Headers() const noexcept -> mhttp::Headers const& { return m_impl->headers; }

        /**
         * @brief Borrow the first case-insensitive response field value.
         * @param name Field name.
         * @param fallback Borrowed default when the field is absent.
         * @return Borrowed field value or fallback, invalidated by relevant field mutation.
         */
        [[nodiscard]] auto Header(
            std::string_view name,
            std::string_view fallback = {}
        ) const noexcept -> std::string_view { return m_impl->headers.Get(name, fallback); }

        /**
         * @brief Borrow the owned binary representation, including bytes suppressed for HEAD or status.
         * @return Body valid until the next content change or destruction.
         */
        [[nodiscard]] auto Body() const noexcept -> std::string const& { return m_impl->body; }

        /**
         * @brief Inspect whether this response uses a deferred streaming representation.
         * @return True after provider registration, including after that provider has been consumed.
         * @note Body remains empty for streams; no complete aggregate representation is retained.
         */
        [[nodiscard]] auto IsStreaming() const noexcept -> bool { return m_impl->streaming; }

        /**
         * @brief Inspect the representation length independently of wire body suppression.
         * @return Buffered byte count, declared stream length, or no value for an unknown stream.
         */
        [[nodiscard]] auto ContentLength() const noexcept -> std::optional<std::uint64_t> {
            return m_impl->streaming ? m_impl->content_length : std::optional<std::uint64_t>{ m_impl->body.size() };
        }

        /**
         * @brief Own a deferred producer with an exact Content-Length for either HTTP version.
         * @param length Exact decoded byte count, including zero.
         * @param provider Nonempty owned producer invoked after final hooks and head transmission.
         * @param content_type Validated media type, or empty to remove Content-Type.
         * @return Success, InvalidRequest, or InvalidHeaders without changing the prior representation.
         * @note HEAD/204/304 skip the producer. Too many or too few bytes, trailers, cancellation,
         *       or timeout close the connection. Each Write remains bounded by server stream limits.
         */
        [[nodiscard]] auto SetContentProvider(
            std::uint64_t length,
            ContentProvider provider,
            std::string_view content_type = "application/octet-stream"
        ) -> Result<void> {
            return m_impl->SetChunkedContentProvider(std::move(provider), content_type, length);
        }

        /**
         * @brief Own a deferred content producer while preserving status and unrelated fields.
         * @param provider Nonempty owned callback, invoked once after final hooks and head transmission.
         * @param content_type Borrowed validated media type, or empty to remove Content-Type.
         * @return Success, InvalidRequest, or InvalidHeaders without changing the prior representation.
         * @note HTTP/1.1 uses chunks; HTTP/1.0 sends raw bytes and closes. HEAD/204/304 skip the
         *       provider entirely. The provider and every awaited write share write_timeout. Errors
         *       after commitment close the connection; error/post-routing hooks are not run again.
         */
        [[nodiscard]] auto SetChunkedContentProvider(
            ContentProvider provider,
            std::string_view content_type = "application/octet-stream"
        ) -> Result<void> { return m_impl->SetChunkedContentProvider(std::move(provider), content_type); }

        /**
         * @brief Set owned binary content and its field value, preserving the current status.
         * @param body Representation moved into this response after validation.
         * @param content_type Validated Content-Type value, or empty to remove that field.
         * @return Success or InvalidHeaders without changing existing content on rejection.
         */
        [[nodiscard]] auto SetContent(
            std::string body,
            std::string_view content_type
        ) -> Result<void> {
            return m_impl->SetContent(std::move(body), content_type);
        }

        /**
         * @brief Serialize JSON into owned content and set application/json without changing status.
         * @tparam T Exact value type supported by the optional JSON adapter.
         * @tparam Adapter Deferred adapter supplied by the json feature.
         * @param value Borrowed JSON tree, fully serialized during this call.
         * @return Success or a serialization error preserving the prior body/headers/status.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto SetJson(T const& value) -> Result<void> {
            auto serialized = Adapter::Serialize(value);
            if (!serialized) { return std::unexpected{ serialized.error() }; }
            return SetContent(std::move(*serialized), "application/json");
        }

        /**
         * @brief Clear representation bytes and Content-Type while setting a status.
         * @param status Known or extension status in [100, 999].
         * @return Success or InvalidRequest without changing existing content on rejection.
         */
        [[nodiscard]] auto SetEmpty(mhttp::Status status) -> Result<void> { return m_impl->SetEmpty(status); }

        /**
         * @brief Replace content with a default plain-text error body and set its status.
         * @param status Status in [400, 599]; exception messages are never included.
         * @return Success or InvalidRequest for a non-error status.
         */
        [[nodiscard]] auto SetError(mhttp::Status status) -> Result<void> { return m_impl->SetError(status); }

        /**
         * @brief Set an empty redirect and a validated Location field.
         * @param url Nonempty relative or absolute reference, copied before content is cleared.
         * @param status Redirect status excluding 304; defaults to 302 Found.
         * @return Success or InvalidUrl, InvalidRequest, or InvalidHeaders without changing state.
         */
        [[nodiscard]] auto SetRedirect(
            std::string_view url,
            mhttp::Status status = mhttp::Status::Found
        ) -> Result<void> { return m_impl->SetRedirect(url, status); }
    };

} // namespace mhttp::server

export namespace mhttp::detail {

    /**
     * @brief Obtain owned protocol metadata for a session without exposing transport implementation state.
     * @internal
     */
    struct ServerResponseAccess {
        /**
         * @brief Move the deferred producer into a stable session frame before invoking it.
         * @param response Serialized response; the streaming representation flag is preserved.
         * @return Owned provider or an empty function for a buffered/already-consumed response.
         */
        [[nodiscard]] static auto TakeProvider(
            server::Response& response
        ) -> server::ContentProvider {
            return std::move(response.m_impl->provider);
        }

        /**
         * @brief Snapshot status and headers for authoritative framing and wire body suppression.
         * @param response Borrowed buffered response, serialized against mutation.
         * @param version Wire version chosen from the request.
         * @return Independently owned metadata with the standard reason phrase selected by serialization.
         */
        [[nodiscard]] static auto Head(
            server::Response const& response,
            mhttp::Version version = mhttp::Version::Http11
        ) -> ResponseHead {
            return { .version = version, .status = response.m_impl->status, .headers = response.m_impl->headers };
        }
    };

} // namespace mhttp::detail
