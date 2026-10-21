/**
 * @file authorization.cppm
 * @brief Validate HTTP authentication tokens shared by the client and built-in server middleware.
 */
module mhttp:utils.authorization;

import std;
import :core.headers;
import :utils.string;

namespace mhttp::detail {

    /**
     * @brief Validate a nonempty RFC 6750 b64token with optional trailing padding.
     * @param token Borrowed credential bytes.
     * @return Whether token contains only the permitted alphabet and trailing equals signs.
     */
    [[nodiscard]] auto is_bearer_token(std::string_view token) noexcept -> bool {
        if (token.empty() || token.front() == '=') { return false; }
        bool padding{};
        for (unsigned char value : token) {
            if (value == '=') { padding = true; continue; }
            bool const allowed = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
                || (value >= '0' && value <= '9') || std::string_view{ "-._~+/" }.contains(static_cast<char>(value));
            if (padding || !allowed) { return false; }
        }
        return true;
    }

    /**
     * @brief Extract one unambiguous credential after a case-insensitive authentication scheme.
     * @param headers Borrowed validated request fields.
     * @param scheme Expected scheme.
     * @return Borrowed token, absent for duplicate fields, other schemes, or a missing token/separator.
     */
    [[nodiscard]] auto authorization_token(
        Headers const& headers,
        std::string_view scheme
    ) noexcept -> std::optional<std::string_view> {
        if (headers.Count("Authorization") != 1) { return {}; }
        auto value = headers.Get("Authorization");
        auto const separator = value.find_first_of(" \t");
        if (separator == std::string_view::npos || !utils::iequals(value.substr(0, separator), scheme)) { return {}; }
        value.remove_prefix(separator);
        value = utils::trim_ows(value);
        if (value.empty()) { return {}; }
        return value;
    }

    /**
     * @brief Build a quoted realm without permitting header injection.
     * @param scheme Constant authentication scheme supplied by the middleware factory.
     * @param realm Borrowed user-visible protection space, copied and quoted here.
     * @return Owned challenge or InvalidHeaders for unsafe bytes.
     */
    [[nodiscard]] auto make_auth_challenge(
        std::string_view scheme,
        std::string_view realm
    ) -> Result<std::string> {
        auto challenge = std::string{ scheme } + " realm=\"";
        for (char value : realm) {
            if (value == '"' || value == '\\') { challenge += '\\'; }
            challenge += value;
        }
        challenge += '"';
        if (scheme == "Basic") { challenge += ", charset=\"UTF-8\""; }
        Headers validated;
        auto result = validated.Set("WWW-Authenticate", challenge);
        if (!result) { return std::unexpected{ result.error() }; }
        return challenge;
    }

    /**
     * @brief Compare equal-length credential bytes without a data-dependent early exit.
     * @param left Borrowed presented credential.
     * @param right Borrowed configured credential.
     * @return Equality; differing lengths are rejected immediately.
     * @note This is an ordinary C++ comparison, without a platform-level constant-time guarantee.
     */
    [[nodiscard]] auto equal_credentials(std::string_view left, std::string_view right) noexcept -> bool {
        if (left.size() != right.size()) { return false; }
        unsigned difference{};
        for (std::size_t i{}; i < left.size(); ++i) {
            difference |= static_cast<unsigned char>(left[i]) ^ static_cast<unsigned char>(right[i]);
        }
        return difference == 0;
    }

} // namespace mhttp::detail
