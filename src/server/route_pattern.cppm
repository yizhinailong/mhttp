/**
 * @file route_pattern.cppm
 * @brief Validate route segments and bound regular expressions before building a routing tree.
 */
export module mhttp:server.route_pattern;

import std;
import :utils.string;
import :utils.percent;
export import :core.error;

export namespace mhttp::detail {

    /**
     * @brief Bound regular-expression admission and untrusted matching subjects.
     * @internal
     */
    struct RouteLimits {
        std::size_t regex_pattern_bytes{ 256 }; ///< Maximum source bytes per regular expression.
        std::size_t regex_subject_bytes{ 1024 }; ///< Maximum decoded bytes in a regular-expression subject.
    };

    /**
     * @brief Distinguish the four routing-tree edge types.
     * @internal
     */
    enum class RouteSegmentKind {
        Static, ///< Exact decoded bytes.
        Regex, ///< Named whole-segment regular expression.
        Parameter, ///< Named segment, including an empty segment.
        Wildcard, ///< All remaining segments, under the name *.
    };

    /**
     * @brief Own one validated route edge and any precompiled expression.
     * @internal
     */
    struct RouteSegment {
        RouteSegmentKind kind{ RouteSegmentKind::Static }; ///< Matching category.
        std::string key; ///< Exact registration spelling identifying this edge.
        std::string name; ///< Parameter name, empty for static edges.
        std::optional<std::regex> pattern; ///< Compiled expression for regex edges only.
    };

    /**
     * @brief Reject quantified groups containing quantifiers or alternation, including nested groups.
     * @param pattern Borrowed ECMAScript expression; escaped bytes and character classes are skipped.
     * @return Whether the expression has a rejected backtracking shape.
     * @note This is conservative admission checking, not a linear-time matching guarantee.
     * @internal
     */
    [[nodiscard]] auto has_redos_shape(std::string_view pattern) -> bool {
        std::vector<bool> groups;
        bool in_class{};
        for (std::size_t index{}; index < pattern.size(); ++index) {
            auto const byte = pattern[index];
            if (byte == '\\') { ++index; continue; }
            if (in_class) {
                if (byte == ']') { in_class = false; }
                continue;
            }
            if (byte == '[') { in_class = true; continue; }
            if (byte == '(') {
                groups.push_back(false);
                if (index + 2 < pattern.size() && pattern[index + 1] == '?'
                    && (pattern[index + 2] == ':' || pattern[index + 2] == '=' || pattern[index + 2] == '!')) {
                    index += 2;
                }
                continue;
            }
            if (byte == ')') {
                if (groups.empty()) { return false; } // Invalid syntax is rejected by std::regex.
                bool const risky = groups.back();
                groups.pop_back();
                bool const quantified = index + 1 < pattern.size()
                    && std::string_view{ "*+?{" }.contains(pattern[index + 1]);
                if (risky && quantified) { return true; }
                if (!groups.empty() && (risky || quantified)) { groups.back() = true; }
                continue;
            }
            if (!groups.empty() && std::string_view{ "|*+?{" }.contains(byte)) { groups.back() = true; }
        }
        return false;
    }

    /**
     * @brief Split an encoded path before decoding, retaining empty and trailing segments.
     * @param encoded_path Borrowed path without a query or fragment.
     * @return Owned decoded segments; encoded / remains part of one segment, and + stays literal.
     * @internal
     */
    [[nodiscard]] auto decode_path_segments(std::string_view encoded_path) -> std::vector<std::string> {
        std::vector<std::string> result;
        for (auto segment : utils::split(encoded_path, '/')) {
            result.push_back(UrlDecode(segment, UrlEncoding::PathSegment));
        }
        return result;
    }

    /**
     * @brief Validate every segment before a route registration can change the tree.
     * @param path Decoded registration pattern beginning with /, or the literal asterisk target.
     * @param limits Regular-expression source and subject bounds.
     * @return Owned segments, or InvalidRequest for invalid syntax, names, expressions, or limits.
     * @note Parameter names must be distinct nonempty HTTP tokens other than *. Wildcards are terminal.
     *       Static registration bytes are already decoded; percent decoding applies only to request paths.
     * @internal
     */
    [[nodiscard]] auto parse_route_pattern(
        std::string_view path,
        RouteLimits limits = {}
    ) -> Result<std::vector<RouteSegment>> {
        if (!path.starts_with('/') && path != "*") {
            return std::unexpected{ Error::InvalidRequest };
        }
        auto const parts = utils::split(path, '/');
        std::vector<RouteSegment> result;
        std::unordered_set<std::string> names;
        for (std::size_t index{}; index < parts.size(); ++index) {
            auto const part = parts[index];
            RouteSegment segment{ .key = std::string{ part } };
            if (part == "*" && path != "*") {
                if (index + 1 != parts.size()) { return std::unexpected{ Error::InvalidRequest }; }
                segment.kind = RouteSegmentKind::Wildcard;
                segment.name = "*";
            } else if (part.starts_with(':')) {
                segment.kind = RouteSegmentKind::Parameter;
                segment.name = part.substr(1);
            } else if (part.starts_with('{')) {
                auto const colon = part.find(':');
                if (!part.ends_with('}') || colon == std::string_view::npos || colon >= part.size() - 1) {
                    return std::unexpected{ Error::InvalidRequest };
                }
                segment.kind = RouteSegmentKind::Regex;
                segment.name = part.substr(1, colon - 1);
                auto const expression = part.substr(colon + 1, part.size() - colon - 2);
                if (expression.size() > limits.regex_pattern_bytes || has_redos_shape(expression)) {
                    return std::unexpected{ Error::InvalidRequest };
                }
                try {
                    segment.pattern.emplace(expression.begin(), expression.end(),
                        std::regex::ECMAScript | std::regex::nosubs | std::regex::optimize);
                } catch (std::regex_error const&) {
                    return std::unexpected{ Error::InvalidRequest };
                }
            } else if (part.find_first_of("?#") != std::string_view::npos) {
                return std::unexpected{ Error::InvalidRequest };
            }
            if (segment.kind == RouteSegmentKind::Parameter || segment.kind == RouteSegmentKind::Regex) {
                if (!utils::is_field_name(segment.name) || segment.name == "*"
                    || !names.insert(segment.name).second) {
                    return std::unexpected{ Error::InvalidRequest };
                }
            }
            result.push_back(std::move(segment));
        }
        return result;
    }

} // namespace mhttp::detail
