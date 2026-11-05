/**
 * @file proxy_common.cppm
 * @brief Shared reverse proxy path rewriting and header filtering.
 */
module;

export module mhttp:proxy_common;

import std;
import :strings;
import :headers;
import :request;

namespace mhttp::detail {

    /**
     * @brief Copy end-to-end fields after removing every Connection nomination.
     */
    auto proxy_headers(Headers const& source) -> Headers {
        Headers result;
        std::vector<std::string> nominated;
        for (auto const header : source) {
            result.Insert(header.name_string, header.value);
            if (iequals(header.name_string, "Connection")) {
                for (auto const token : split(header.value, ",")) {
                    if (auto const name{ trim(token) }; !name.empty()) {
                        nominated.emplace_back(name);
                    }
                }
            }
        }
        for (auto const& name : nominated) {
            result.Erase(name);
        }
        for (std::string_view const name : { "Connection", "Keep-Alive", "Proxy-Connection", "Proxy-Authenticate", "Proxy-Authorization", "TE", "Trailer", "Transfer-Encoding", "Upgrade" }) {
            result.Erase(name);
        }
        return result;
    }

    /**
     * @brief Validate and normalize a static route prefix before registration.
     */
    auto proxy_prefix(std::string prefix) -> std::string {
        if (prefix.empty() || prefix.front() != '/' || prefix.find_first_of("*:{}?#%\\") != std::string::npos || prefix.find("//") != std::string::npos || std::ranges::any_of(prefix, [](unsigned char c) { return c <= 32 || c == 127; })) {
            throw std::invalid_argument{ "mhttp: proxy requires a literal path prefix" };
        }
        while (prefix.size() > 1 && prefix.back() == '/') {
            prefix.pop_back();
        }
        return prefix;
    }

    /**
     * @brief Replace matched raw path segments, preserving encoded reserved bytes and the query.
     */
    auto proxy_target(
        Request const& request,
        std::string_view prefix,
        std::string_view base_path,
        bool strip_prefix
    ) -> std::string {
        auto path{ request.Path() };
        if (strip_prefix && prefix != "/") {
            auto const segments{ std::ranges::count(prefix, '/') };
            std::size_t position{ 0 };
            for (std::ptrdiff_t i{ 0 }; i < segments; ++i) {
                position = path.find_first_not_of('/', position);
                position = position == std::string_view::npos ? path.size() : path.find('/', position);
                if (position == std::string_view::npos) {
                    position = path.size();
                }
            }
            path.remove_prefix(position);
        }
        return std::string{ base_path } + std::string{ path.empty() ? "/" : path } + std::string{ request.Target().substr(request.Path().size()) };
    }

} // namespace mhttp::detail
