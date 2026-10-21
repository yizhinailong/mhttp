/**
 * @file cors.cppm
 * @brief Provide validated CORS policy, preflight short circuits, credentials, and cache variation.
 */
export module mhttp:server.cors;

import std;
import :utils.string;
import :core.url;
export import :server.request;
export import :server.response;

namespace mhttp::detail {

    /**
     * @brief Validate a serialized tuple origin or the opaque-origin spelling null.
     * @param origin Borrowed serialized origin, without a trailing slash, path, query, or fragment.
     * @return Whether the scheme and authority can safely identify a single origin.
     */
    [[nodiscard]] auto is_cors_origin(std::string_view origin) -> bool {
        if (origin == "null") { return true; }
        auto const separator = origin.find("://");
        if (separator == std::string_view::npos || separator == 0) { return false; }
        auto const scheme = origin.substr(0, separator);
        auto alpha = [](char value) { return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z'); };
        if (!alpha(scheme.front()) || !std::ranges::all_of(scheme, [alpha](char value) {
            return alpha(value) || (value >= '0' && value <= '9') || value == '+' || value == '-' || value == '.';
        })) { return false; }
        auto const authority = origin.substr(separator + 3);
        if (authority.empty() || authority.find_first_of("/?#") != std::string_view::npos) { return false; }
        return Url::Parse("http://" + std::string{ authority }).has_value();
    }

    /**
     * @brief Join validated token lists in configured order for response fields.
     * @param values Owned configured or parsed tokens.
     * @return Comma/space-delimited field value.
     */
    [[nodiscard]] auto join_cors_tokens(std::vector<std::string> const& values) -> std::string {
        std::string result;
        for (auto const& value : values) {
            if (!result.empty()) { result += ", "; }
            result += value;
        }
        return result;
    }

    /**
     * @brief Merge a case-insensitive Vary token without overwriting existing response cache variation.
     * @param headers Mutable response fields.
     * @param token Constant request-field name controlling response selection.
     */
    auto add_cors_vary(Headers& headers, std::string_view token) -> void {
        std::string existing;
        for (auto value : headers.Values("Vary")) {
            for (auto part : utils::split(value, ',')) {
                part = utils::trim_ows(part);
                if (part == "*" || utils::iequals(part, token)) { return; }
            }
            if (!existing.empty()) { existing += ", "; }
            existing += value;
        }
        if (!existing.empty()) { existing += ", "; }
        existing += token;
        static_cast<void>(headers.Set("Vary", existing));
    }

} // namespace mhttp::detail

export namespace mhttp::server {

    /**
     * @brief Configure an owned origin allowlist, methods, request/exposed fields, credentials, and preflight caching.
     * @note An empty allowlist denies every origin. A sole * permits any syntactically valid origin;
     *       credentials then reflect that individual origin instead of emitting the forbidden wildcard combination.
     */
    struct CorsOptions {
        std::vector<std::string> origins{ "*" }; ///< Exact serialized origins or a sole wildcard.
        std::vector<std::string> methods{ "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" }; ///< Case-sensitive methods or *.
        std::vector<std::string> headers{ "Origin", "Content-Type", "Accept", "Authorization" }; ///< Case-insensitive names; * excludes Authorization unless listed explicitly.
        std::vector<std::string> expose_headers; ///< Response names visible to browser script.
        bool credentials{}; ///< Emit Allow-Credentials: true only for an allowed origin.
        std::chrono::seconds max_age{}; ///< Nonnegative preflight cache duration; zero omits Max-Age.
    };

    /**
     * @brief Apply immutable CORS response policy and answer valid OPTIONS preflights before later aspects.
     * @note Use globally before authentication to let preflights bypass credentials. Ordinary cross-origin
     *       requests still require authentication; disallowed origins receive no CORS permission fields.
     */
    class Cors {
    private:
        CorsOptions m_options; ///< Owned immutable policy after Create.
        std::string m_methods; ///< Prejoined configured methods.
        std::string m_headers; ///< Prejoined configured request-field names.
        std::string m_expose; ///< Prejoined exposed response-field names.

        /**
         * @brief Retain validated configuration and precompute reusable response values.
         * @param options Validated owned policy.
         */
        explicit Cors(CorsOptions options) : m_options{ std::move(options) },
            m_methods{ detail::join_cors_tokens(m_options.methods) },
            m_headers{ detail::join_cors_tokens(m_options.headers) },
            m_expose{ detail::join_cors_tokens(m_options.expose_headers) } {}

        /**
         * @brief Match one valid origin against the immutable policy.
         * @param origin Serialized origin, validated by the caller.
         * @return Whether this origin is allowed.
         */
        [[nodiscard]] auto allowsOrigin(std::string_view origin) const -> bool {
            return std::ranges::contains(m_options.origins, "*") || std::ranges::contains(m_options.origins, origin);
        }

        /**
         * @brief Apply permissions and preserve cache variation before/after handlers and short circuits.
         * @param request Borrowed immutable origin fields.
         * @param response Borrowed response whose existing Vary fields are preserved.
         */
        auto apply(Request const& request, Response& response) const -> void {
            detail::add_cors_vary(response.Headers(), "Origin");
            auto origin = request.Header("Origin");
            if (request.Headers().Count("Origin") != 1 || !detail::is_cors_origin(origin) || !allowsOrigin(origin)) { return; }
            bool const wildcard = std::ranges::contains(m_options.origins, "*") && !m_options.credentials;
            static_cast<void>(response.Headers().Set("Access-Control-Allow-Origin", wildcard ? "*" : origin));
            if (m_options.credentials) { static_cast<void>(response.Headers().Set("Access-Control-Allow-Credentials", "true")); }
            if (!m_expose.empty()) { static_cast<void>(response.Headers().Set("Access-Control-Expose-Headers", m_expose)); }
        }

    public:
        /**
         * @brief Validate and own a CORS policy before registration.
         * @param options Owned policy; origins must be serialized origins without paths or surrounding whitespace.
         * @return Middleware or InvalidRequest for invalid origins, tokens, wildcard combinations, or negative cache age.
         */
        [[nodiscard]] static auto Create(CorsOptions options = {}) -> Result<Cors> {
            if (options.max_age.count() < 0 || (std::ranges::contains(options.origins, "*") && options.origins.size() != 1)
                || (options.credentials && std::ranges::contains(options.expose_headers, "*"))) {
                return std::unexpected{ Error::InvalidRequest };
            }
            for (auto const& origin : options.origins) {
                if (origin != "*" && !detail::is_cors_origin(origin)) { return std::unexpected{ Error::InvalidRequest }; }
            }
            for (auto const* list : { &options.methods, &options.headers, &options.expose_headers }) {
                for (auto const& token : *list) {
                    if (!utils::is_field_name(token)) { return std::unexpected{ Error::InvalidRequest }; }
                }
            }
            return Cors{ std::move(options) };
        }

        /**
         * @brief Apply permissions and short-circuit allowed preflights with an empty 204 response.
         * @param request Borrowed request; preflights require OPTIONS, Origin, and Access-Control-Request-Method.
         * @param response Borrowed response; malformed preflights return 400 and policy denials return 403.
         * @return True for ordinary requests, false for completed or rejected preflights/invalid Origin fields.
         */
        auto Before(Request& request, Response& response) const -> bool {
            apply(request, response);
            if (!request.Headers().Has("Origin")) { return true; }
            if (request.Headers().Count("Origin") != 1 || !detail::is_cors_origin(request.Header("Origin"))) {
                static_cast<void>(response.SetError(Status::BadRequest));
                return false;
            }
            if (request.Method() != Method::Options || !request.Headers().Has("Access-Control-Request-Method")) { return true; }
            detail::add_cors_vary(response.Headers(), "Access-Control-Request-Method");
            detail::add_cors_vary(response.Headers(), "Access-Control-Request-Headers");
            auto const method = request.Header("Access-Control-Request-Method");
            if (request.Headers().Count("Access-Control-Request-Method") != 1 || !utils::is_field_name(method)) {
                static_cast<void>(response.SetError(Status::BadRequest));
                return false;
            }
            bool allowed = allowsOrigin(request.Header("Origin"))
                && (std::ranges::contains(m_options.methods, "*") || std::ranges::contains(m_options.methods, method));
            std::vector<std::string> requested;
            for (auto value : request.Headers().Values("Access-Control-Request-Headers")) {
                for (auto name : utils::split(value, ',')) {
                    name = utils::trim_ows(name);
                    if (!utils::is_field_name(name)) {
                        static_cast<void>(response.SetError(Status::BadRequest));
                        return false;
                    }
                    bool const listed = std::ranges::any_of(m_options.headers,
                        [name](auto const& item) { return utils::iequals(item, name); });
                    bool const wildcard = std::ranges::contains(m_options.headers, "*") && !utils::iequals(name, "Authorization");
                    allowed = allowed && (listed || wildcard);
                    requested.emplace_back(name);
                }
            }
            if (!allowed) {
                static_cast<void>(response.SetError(Status::Forbidden));
                return false;
            }
            static_cast<void>(response.SetEmpty(Status::NoContent));
            static_cast<void>(response.Headers().Set("Access-Control-Allow-Methods",
                std::ranges::contains(m_options.methods, "*") ? method : std::string_view{ m_methods }));
            auto const headers = std::ranges::contains(m_options.headers, "*") ? detail::join_cors_tokens(requested) : m_headers;
            if (!headers.empty()) { static_cast<void>(response.Headers().Set("Access-Control-Allow-Headers", headers)); }
            if (m_options.max_age.count() > 0) {
                static_cast<void>(response.Headers().Set("Access-Control-Max-Age", std::to_string(m_options.max_age.count())));
            }
            return false;
        }

        /**
         * @brief Reapply origin permissions after handlers/authentication and merge any added Vary fields.
         * @param request Borrowed immutable request.
         * @param response Borrowed completed route response.
         * @return True to continue later After aspects.
         */
        auto After(Request& request, Response& response) const -> bool {
            apply(request, response);
            return true;
        }
    };

} // namespace mhttp::server
