/**
 * @file auth.cppm
 * @brief Provide owned Basic and Bearer authentication aspects for routes and global middleware.
 */
export module mhttp:server.auth;

import std;
import :utils.authorization;
import :utils.base64;
export import :server.request;
export import :server.response;

export namespace mhttp::server {

    /**
     * @brief Authenticate one Basic Authorization field before calling protected handlers.
     * @note Router owns a copy or move of this aspect. Validators may run concurrently, must remain
     *       nonblocking, and must synchronize shared captured state; reference captures remain borrowed.
     */
    class BasicAuth {
    public:
        using Validator = std::function<bool(std::string_view, std::string_view)>; ///< Borrowed user/password valid during invocation only.

    private:
        Validator m_validator; ///< Owned callable shared only through application-provided captures.
        std::string m_challenge; ///< Validated quoted protection-space challenge.

        /**
         * @brief Own a validated callable and challenge after factory validation.
         * @param validator Nonempty validator.
         * @param challenge Header-safe challenge.
         */
        BasicAuth(Validator validator, std::string challenge) :
            m_validator{ std::move(validator) }, m_challenge{ std::move(challenge) } {}

    public:
        /**
         * @brief Create Basic authentication using an owned nonblocking credential validator.
         * @param validator Copied/moved callback, receiving decoded credentials split at the first colon.
         * @param realm Protection-space label copied during this call; quotes and backslashes are escaped.
         * @return Valid middleware, InvalidRequest for an empty callback, or InvalidHeaders for unsafe realm bytes.
         */
        [[nodiscard]] static auto Create(
            Validator validator,
            std::string_view realm = "Restricted"
        ) -> Result<BasicAuth> {
            if (!validator) { return std::unexpected{ Error::InvalidRequest }; }
            auto challenge = detail::make_auth_challenge("Basic", realm);
            if (!challenge) { return std::unexpected{ challenge.error() }; }
            return BasicAuth{ std::move(validator), std::move(*challenge) };
        }

        /**
         * @brief Create a fixed credential validator that owns its username and password.
         * @param username Identifier without colons, copied during this call.
         * @param password Password, including optional colons, copied during this call.
         * @param realm Copied challenge realm.
         * @return Middleware or a validation failure; no borrowed credentials are retained.
         */
        [[nodiscard]] static auto Create(
            std::string_view username,
            std::string_view password,
            std::string_view realm = "Restricted"
        ) -> Result<BasicAuth> {
            if (username.contains(':')) { return std::unexpected{ Error::InvalidRequest }; }
            return Create([user = std::string{ username }, secret = std::string{ password }](auto presented_user, auto presented_password) {
                bool const user_matches = detail::equal_credentials(presented_user, user);
                bool const password_matches = detail::equal_credentials(presented_password, secret);
                return user_matches && password_matches;
            }, realm);
        }

        /**
         * @brief Allow validated credentials or short-circuit with 401 and WWW-Authenticate.
         * @param request Borrowed complete request; malformed/duplicate authorization never reaches the validator.
         * @param response Borrowed response, changed only when authentication fails.
         * @return True to continue the chain, false after publishing the authentication challenge.
         * @note Validator exceptions propagate into the router's normal exception handling.
         */
        auto Before(Request& request, Response& response) const -> bool {
            if (auto token = detail::authorization_token(request.Headers(), "Basic")) {
                if (auto decoded = Base64Decode(*token)) {
                    auto const separator = decoded->find(':');
                    if (separator != std::string::npos && m_validator(std::string_view{ *decoded }.substr(0, separator),
                        std::string_view{ *decoded }.substr(separator + 1))) { return true; }
                }
            }
            static_cast<void>(response.SetError(Status::Unauthorized));
            static_cast<void>(response.Headers().Set("WWW-Authenticate", m_challenge));
            return false;
        }
    };

    /**
     * @brief Authenticate an RFC 6750 Bearer token using a nonblocking validator or owned fixed secret.
     * @note Validators may run concurrently and must not block I/O threads. The token view is borrowed
     *       only for the call; synchronize shared captures and copy data needed beyond invocation.
     */
    class BearerAuth {
    public:
        using Validator = std::function<bool(std::string_view)>; ///< Owned callback borrowing the presented token.

    private:
        Validator m_validator; ///< Owned validator.
        std::string m_challenge; ///< Validated quoted protection-space challenge.

        /**
         * @brief Own validated authentication state.
         * @param validator Nonempty validator.
         * @param challenge Header-safe challenge.
         */
        BearerAuth(Validator validator, std::string challenge) :
            m_validator{ std::move(validator) }, m_challenge{ std::move(challenge) } {}

    public:
        /**
         * @brief Create Bearer authentication using an owned nonblocking validator.
         * @param validator Callable receiving one syntactically valid token.
         * @param realm Protection-space label copied and escaped during this call.
         * @return Middleware, InvalidRequest for an empty validator, or InvalidHeaders for an unsafe realm.
         */
        [[nodiscard]] static auto Create(
            Validator validator,
            std::string_view realm = "Restricted"
        ) -> Result<BearerAuth> {
            if (!validator) { return std::unexpected{ Error::InvalidRequest }; }
            auto challenge = detail::make_auth_challenge("Bearer", realm);
            if (!challenge) { return std::unexpected{ challenge.error() }; }
            return BearerAuth{ std::move(validator), std::move(*challenge) };
        }

        /**
         * @brief Create a validator retaining an independent copy of a fixed Bearer token.
         * @param token Nonempty RFC 6750 b64token; copied before returning.
         * @param realm Copied challenge realm.
         * @return Middleware or InvalidHeaders for an invalid token/realm.
         */
        [[nodiscard]] static auto Create(
            std::string_view token,
            std::string_view realm = "Restricted"
        ) -> Result<BearerAuth> {
            if (!detail::is_bearer_token(token)) { return std::unexpected{ Error::InvalidHeaders }; }
            return Create([secret = std::string{ token }](auto presented) {
                return detail::equal_credentials(presented, secret);
            }, realm);
        }

        /**
         * @brief Continue for one valid Bearer credential or short-circuit with 401 and a challenge.
         * @param request Borrowed request, retaining authorization storage through validator invocation.
         * @param response Borrowed response changed only on authentication failure.
         * @return True when authorized, false with WWW-Authenticate when absent, malformed, or rejected.
         */
        auto Before(Request& request, Response& response) const -> bool {
            if (auto token = detail::authorization_token(request.Headers(), "Bearer")) {
                if (detail::is_bearer_token(*token) && m_validator(*token)) { return true; }
            }
            static_cast<void>(response.SetError(Status::Unauthorized));
            static_cast<void>(response.Headers().Set("WWW-Authenticate", m_challenge));
            return false;
        }
    };

} // namespace mhttp::server
