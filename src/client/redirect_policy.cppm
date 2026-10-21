/**
 * @file redirect_policy.cppm
 * @brief Resolve bounded redirects and rewrite owned request data independently of transport state.
 */
export module mhttp:client.redirect_policy;

import std;
export import :core.url;
export import :client.request;

export namespace mhttp::detail {

    /**
     * @brief Compare normalized HTTP origins, including effective ports.
     * @param left Borrowed normalized URL.
     * @param right Borrowed normalized URL.
     * @return Whether both URLs share a scheme, host, and port.
     * @internal
     */
    [[nodiscard]] auto same_client_origin(Url const& left, Url const& right) noexcept -> bool {
        return left.Scheme() == right.Scheme() && left.Host() == right.Host() && left.Port() == right.Port();
    }

    /**
     * @brief Select a followable Location and enforce the configured redirect limit.
     * @param current Effective URL of the request producing this response.
     * @param status Received status.
     * @param headers Received fields; duplicate Locations are rejected when following is enabled.
     * @param maximum Maximum followed hops, or zero to disable following.
     * @param followed Number already followed in this Send operation.
     * @return Destination, no redirect, or an invalid URL/header/scheme/redirect-count error.
     * @internal
     */
    [[nodiscard]] auto client_redirect_target(
        Url const& current,
        Status status,
        Headers const& headers,
        std::size_t maximum,
        std::size_t followed
    ) -> Result<std::optional<Url>> {
        if (maximum == 0 || (status != Status::MovedPermanently && status != Status::Found
            && status != Status::SeeOther && status != Status::TemporaryRedirect && status != Status::PermanentRedirect)
            || headers.Get("Location").empty()) { return std::optional<Url>{}; }
        if (headers.Count("Location") != 1) { return std::unexpected{ Error::InvalidHeaders }; }
        if (followed >= maximum) { return std::unexpected{ Error::ExceedRedirectCount }; }
        auto resolved = Url::Resolve(current, headers.Get("Location"));
        if (!resolved) { return std::unexpected{ resolved.error() }; }
        return std::optional{ std::move(*resolved) };
    }

    /**
     * @brief Rewrite a retained request for the next hop, stripping cross-origin credentials permanently.
     * @param request Owned request reused by the current Send only.
     * @param defaults Mutable issue-time fallback snapshot; the client's configuration is never changed.
     * @param current Previous effective URL.
     * @param destination Resolved next URL; its target already contains the next query.
     * @param status Redirect status determining method/body rewriting.
     * @note A 303 always selects GET; 301/302 select GET except for HEAD. 307/308 preserve the body.
     * @internal
     */
    auto redirect_client_request(
        client::Request& request,
        Headers& defaults,
        Url const& current,
        Url const& destination,
        Status status
    ) -> void {
        if (!same_client_origin(current, destination)) {
            for (auto name : { "Host", "Authorization", "Proxy-Authorization", "Cookie" }) {
                request.Headers().Erase(name);
                defaults.Erase(name);
            }
        }
        if (status == Status::SeeOther || ((status == Status::MovedPermanently || status == Status::Found)
            && request.Method() != Method::Head)) {
            static_cast<void>(request.SetMethod(Method::Get));
            static_cast<void>(request.SetContent({}));
            for (auto name : { "Content-Type", "Content-Encoding", "Expect" }) {
                request.Headers().Erase(name);
                defaults.Erase(name);
            }
        }
        request.SetTarget(destination.Target());
        request.Query().Clear();
    }

} // namespace mhttp::detail
