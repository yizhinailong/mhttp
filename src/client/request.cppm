/**
 * @file request.cppm
 * @brief Expose an owned outgoing request with ordered headers, additional query parameters, and binary content.
 */
export module mhttp:client.request;

import std;
import :client.impl.request;
export import :core.message;
export import :core.query;
export import :core.json_support;

namespace mhttp::impl {

    class ClientImpl;

} // namespace mhttp::impl

export namespace mhttp::client {

    /**
     * @brief Own move-only outgoing data that transfers completely into a client Send operation.
     * @note Views borrow this request until relevant mutation or destruction. Do not use retained
     *       views after transferring the request to Send. Concurrent mutation requires synchronization.
     *       A moved-from request may only be destroyed or assigned. Construction does not perform I/O;
     *       the sending boundary validates the complete method/target and computes authoritative framing.
     */
    class Request {
    private:
        std::shared_ptr<impl::ClientRequestImpl> m_impl; ///< Unique public ownership of outgoing state.
        friend class impl::ClientImpl;

    public:
        /**
         * @brief Own a raw method token and target without resolving or connecting to a peer.
         * @param method Case-sensitive token, including extension methods; Send validates it.
         * @param target Raw path/query target; Send handles fragments and outgoing escaping.
         */
        Request(std::string method, std::string target) :
            m_impl{ std::make_shared<impl::ClientRequestImpl>(std::move(method), std::move(target)) } {}

        /**
         * @brief Create an empty GET for the root path, or own another known method and target.
         * @param method Known method; an invalid enumerator is rejected at the sending boundary.
         * @param target Owned path/query target, defaulting to root.
         */
        explicit Request(mhttp::Method method = mhttp::Method::Get, std::string target = "/") :
            Request{ std::string{ ToString(method) }, std::move(target) } {}

        /**
         * @brief Prevent aliasing mutable outgoing state.
         * @param other Unused source.
         */
        Request(Request const& other) = delete;

        /**
         * @brief Prevent replacement by a shared mutable request alias.
         * @param other Unused source.
         * @return Not callable.
         */
        auto operator=(Request const& other) -> Request& = delete;

        /**
         * @brief Transfer owned outgoing storage without relocating its contents.
         * @param other Previous owner, left suitable for destruction or assignment.
         */
        Request(Request&& other) noexcept = default;

        /**
         * @brief Release current storage and take another request's state.
         * @param other Previous owner.
         * @return This request.
         */
        auto operator=(Request&& other) noexcept -> Request& = default;

        /**
         * @brief Recognize a known method while preserving extension tokens.
         * @return Known method or nullopt for an extension/invalid constructor token.
         */
        [[nodiscard]] auto Method() const noexcept -> std::optional<mhttp::Method> { return ParseMethod(m_impl->method); }

        /**
         * @brief Borrow the exact case-sensitive outgoing method token.
         * @return View valid until a method mutation or destruction.
         */
        [[nodiscard]] auto MethodString() const noexcept -> std::string_view { return m_impl->method; }

        /**
         * @brief Replace a method after complete token validation.
         * @param method Borrowed known or extension method token.
         * @return Success or InvalidRequest without changing state.
         */
        [[nodiscard]] auto SetMethod(std::string_view method) -> Result<void> { return m_impl->SetMethod(method); }

        /**
         * @brief Replace the current method with a valid known enumerator.
         * @param method Supported method value.
         * @return Success or InvalidRequest for an invalid enumerator.
         */
        [[nodiscard]] auto SetMethod(mhttp::Method method) -> Result<void> { return SetMethod(ToString(method)); }

        /**
         * @brief Borrow the original target before sending normalization.
         * @return View valid until target mutation or destruction.
         */
        [[nodiscard]] auto Target() const noexcept -> std::string_view { return m_impl->target; }

        /**
         * @brief Own a replacement target for later validation by Send.
         * @param target Owned raw path/query target.
         */
        auto SetTarget(std::string target) -> void { m_impl->target = std::move(target); }

        /**
         * @brief Borrow mutable additional query parameters in caller insertion order.
         * @return Collection appended after any query already in Target when sending.
         */
        [[nodiscard]] auto Query() noexcept -> QueryParams& { return m_impl->query; }

        /**
         * @brief Borrow read-only additional query parameters.
         * @return Collection owned by this request.
         */
        [[nodiscard]] auto Query() const noexcept -> QueryParams const& { return m_impl->query; }

        /**
         * @brief Borrow validated mutable outgoing fields.
         * @return Owned field collection; mutations reject CRLF and invalid names.
         */
        [[nodiscard]] auto Headers() noexcept -> mhttp::Headers& { return m_impl->headers; }

        /**
         * @brief Borrow read-only outgoing fields.
         * @return Collection owned by this request.
         */
        [[nodiscard]] auto Headers() const noexcept -> mhttp::Headers const& { return m_impl->headers; }

        /**
         * @brief Borrow the first case-insensitive outgoing field.
         * @param name Field name.
         * @param fallback Borrowed default for a missing field.
         * @return Borrowed value, invalidated by relevant field mutation.
         */
        [[nodiscard]] auto Header(
            std::string_view name,
            std::string_view fallback = {}
        ) const noexcept -> std::string_view { return m_impl->headers.Get(name, fallback); }

        /**
         * @brief Borrow the outgoing representation, including embedded NUL bytes.
         * @return Bytes owned by this request.
         */
        [[nodiscard]] auto Body() const noexcept -> std::string const& { return m_impl->body; }

        /**
         * @brief Serialize an optional JSON value into owned request content with application/json.
         * @tparam T Exact value type supplied by the optional JSON adapter.
         * @tparam Adapter Deferred adapter supplied by the json feature.
         * @param value Borrowed tree, serialized synchronously; no reference survives this call.
         * @return Success or a serialization error preserving the previous request state.
         */
        template <typename T, typename Adapter = detail::JsonAdapter<>>
            requires detail::JsonValue<T, Adapter>
        [[nodiscard]] auto SetJson(T const& value) -> Result<void> {
            auto serialized = Adapter::Serialize(value);
            if (!serialized) { return std::unexpected{ serialized.error() }; }
            return SetContent(std::move(*serialized), "application/json");
        }

        /**
         * @brief Borrow the complete optional media type.
         * @return Content-Type or an empty view when absent.
         */
        [[nodiscard]] auto ContentType() const noexcept -> std::string_view { return Header("Content-Type"); }

        /**
         * @brief Own binary content and an optional media type without inferring one.
         * @param body Owned representation.
         * @param content_type Validated field value; empty removes Content-Type.
         * @return Success or InvalidHeaders without changing the prior content or fields.
         */
        [[nodiscard]] auto SetContent(
            std::string body,
            std::string_view content_type = {}
        ) -> Result<void> { return m_impl->SetContent(std::move(body), content_type); }
    };

} // namespace mhttp::client
