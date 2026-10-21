/**
 * @file request_policy.cppm
 * @brief Prepare complete bounded HTTP client requests without performing network operations.
 */
export module mhttp:client.request_policy;

import std;
export import :client.options;
export import :client.request;
export import :core.url;
export import :core.serializer;
import :utils.percent;

export namespace mhttp::detail {

    /**
     * @brief Own prepared wire metadata and the original request through the entire send operation.
     * @note Send head.bytes before request.Body(). Keeping their storage separate allows the transport
     *       to distinguish a failed zero-byte head write from an unsafe partially transmitted request.
     * @internal
     */
    struct PreparedClientRequest {
        client::Request request; ///< Original owned data; body stays alive without copying or borrowing callers.
        SerializedHead head; ///< Validated head with authoritative framing and connection policy.
        std::string target; ///< Final encoded origin-form target, including all query parameters, without fragment.
    };

    /**
     * @brief Validate a raw host/port constructor pair without accepting URL or authority syntax as a host.
     * @param host Borrowed hostname or IPv6 literal, optionally enclosed in brackets.
     * @param port TCP port; zero is syntactically valid and may fail later at connection establishment.
     * @return Owned normalized HTTP origin or InvalidUrl; no name resolution is attempted.
     * @internal
     */
    [[nodiscard]] auto client_origin(std::string_view host, std::uint16_t port) -> Result<Url> {
        if (host.starts_with('[') && host.ends_with(']')) {
            host.remove_prefix(1);
            host.remove_suffix(1);
            if (!is_url_ipv6(host)) { return std::unexpected{ Error::InvalidUrl }; }
        } else if (host.contains(':')) {
            if (!is_url_ipv6(host)) { return std::unexpected{ Error::InvalidUrl }; }
        } else if (!is_url_host(host)) { return std::unexpected{ Error::InvalidUrl }; }
        return Url::Parse("http://" + MakeHostValue(host, port) + '/');
    }

    /**
     * @brief Normalize a target and append encoded parameters in their original insertion order.
     * @param request Borrowed outgoing data, including raw target and additional binary query values.
     * @param origin Validated base URL, whose target is used for an empty or fragment-only target.
     * @return Owned target without a fragment; the serializer performs final syntax checks and escaping.
     * @internal
     */
    [[nodiscard]] auto client_request_target(
        client::Request const& request,
        Url const& origin
    ) -> std::string {
        auto const raw = request.Target().substr(0, request.Target().find('#'));
        auto target = raw.empty() ? origin.Target() : std::string{ raw };
        bool has_query = target.contains('?');
        for (auto const& [name, value] : request.Query()) {
            if (!has_query) { target += '?'; has_query = true; }
            else if (target.find('?') != target.size() - 1 && !target.ends_with('&')) { target += '&'; }
            target += UrlEncode(name, UrlEncoding::QueryComponent);
            target += '=';
            target += UrlEncode(value, UrlEncoding::QueryComponent);
        }
        return target;
    }

    /**
     * @brief Produce authoritative outgoing framing, fallback fields, and an owned body before any I/O.
     * @param request Owned request, retained in the success result for the entire subsequent operation.
     * @param origin Validated HTTP destination used for an absent Host and empty target.
     * @param options Borrowed configuration; never modified and not retained.
     * @return Prepared data, validation failure, or BodyTooLarge when the complete wire request exceeds its bound.
     * @note Explicit names suppress every matching default field, preserving duplicate defaults otherwise.
     *       No Accept-Encoding or Content-Type is inferred. Stale CL/TE/Trailer fields are replaced by
     *       the serializer. A request-supplied Host is permitted but must be a single valid authority.
     * @internal
     */
    [[nodiscard]] auto prepare_client_request(
        client::Request request,
        Url const& origin,
        client::ClientOptions const& options = {}
    ) -> Result<PreparedClientRequest> {
        auto valid = validate_client_options(options);
        if (!valid) { return std::unexpected{ valid.error() }; }
        if (request.Body().size() > options.max_request_bytes) {
            return std::unexpected{ Error::BodyTooLarge };
        }
        RequestHead head{ .method = std::string{ request.MethodString() },
            .target = client_request_target(request, origin), .headers = request.Headers() };
        for (auto const& [name, value] : options.default_headers) {
            if (!request.Headers().Has(name)) {
                auto added = head.headers.Add(name, value);
                if (!added) { return std::unexpected{ added.error() }; }
            }
        }
        if (!head.headers.Has("Host")) {
            auto added = head.headers.Add("Host", MakeHostValue(origin.Host(), origin.Port()));
            if (!added) { return std::unexpected{ added.error() }; }
        }
        if (!head.headers.Has("User-Agent") && !options.user_agent.empty()) {
            auto added = head.headers.Add("User-Agent", options.user_agent);
            if (!added) { return std::unexpected{ added.error() }; }
        }
        auto serialized = serialize_request_head(head,
            { .body_bytes = request.Body().size(), .keep_alive = options.keep_alive });
        if (!serialized) { return std::unexpected{ serialized.error() }; }
        if (serialized->bytes.size() > options.max_request_bytes - request.Body().size()) {
            return std::unexpected{ Error::BodyTooLarge };
        }
        return PreparedClientRequest{ std::move(request), std::move(*serialized), encode_request_target(head.target) };
    }

} // namespace mhttp::detail
