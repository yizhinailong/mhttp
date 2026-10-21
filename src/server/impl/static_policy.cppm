/**
 * @file static_policy.cppm
 * @brief Validate portable rooted paths, conditional validators, and bounded byte-range specifications.
 */
module mhttp:server.impl.static_policy;

import std;
import :utils.percent;
import :utils.string;
import :utils.date;
import :core.headers;

namespace mhttp::impl {

    /**
     * @brief Check one portable filename component before any native filesystem operation.
     * @param name Decoded bytes, never decoded a second time.
     * @return False for separators, traversal, alternate streams, control bytes, or DOS device aliases.
     */
    auto static_component(std::string_view name) noexcept -> bool {
        if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') { return false; }
        for (unsigned char byte : name) {
            if (byte < 0x20 || byte == 0x7f || std::string_view{ "/\\:<>\"|?*" }.contains(static_cast<char>(byte))) { return false; }
        }
        auto const stem = name.substr(0, name.find('.'));
        for (auto reserved : { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" }) {
            if (utils::iequals(stem, reserved)) { return false; }
        }
        if (stem.size() == 4 && (utils::iequals(stem.substr(0, 3), "COM") || utils::iequals(stem.substr(0, 3), "LPT"))
            && stem[3] >= '1' && stem[3] <= '9') { return false; }
        return true;
    }

    /**
     * @brief Decode individual URL path segments without treating encoded separators as new segments.
     * @param path Encoded absolute path without query, or decoded registration prefix when decode is false.
     * @param decode Whether to decode each component exactly once.
     * @return Owned portable components, or no value for an unsafe path or more than 128 components.
     */
    auto static_components(
        std::string_view path,
        bool decode = true
    ) -> std::optional<std::vector<std::string>> {
        if (!path.starts_with('/') || path.starts_with("//")) { return std::nullopt; }
        path.remove_prefix(1);
        if (path.ends_with('/')) { path.remove_suffix(1); }
        std::vector<std::string> parts;
        if (path.empty()) { return parts; }
        for (;;) {
            auto const slash = path.find('/');
            auto part = decode ? UrlDecode(path.substr(0, slash)) : std::string{ path.substr(0, slash) };
            if (!static_component(part) || parts.size() == 128) { return std::nullopt; }
            parts.push_back(std::move(part));
            if (slash == path.npos) { return parts; }
            path.remove_prefix(slash + 1);
        }
    }

    /**
     * @brief Store an inclusive satisfiable interval within one immutable selected file length.
     */
    struct StaticRange {
        std::uint64_t first{}; ///< First selected byte.
        std::uint64_t last{}; ///< Last selected byte, inclusive.
    };

    /**
     * @brief Describe ignored Range input, valid partial content, or a rejected/unsatisfiable set.
     */
    struct StaticRanges {
        bool applied{}; ///< False selects the full representation.
        bool rejected{}; ///< True requires 416 rather than a partial representation.
        std::vector<StaticRange> values; ///< Satisfiable ranges in request order.
    };

    /**
     * @brief Parse decimal byte positions without losing ordering or overflowing on arbitrary digit counts.
     * @param text Nonempty decimal digits, retained by the caller.
     * @return Saturated numeric value and normalized digits, or no value for invalid syntax.
     */
    auto static_decimal(
        std::string_view text
    ) -> std::optional<std::pair<std::uint64_t, std::string_view>> {
        if (text.empty()) { return std::nullopt; }
        std::uint64_t number{};
        for (auto byte : text) {
            if (byte < '0' || byte > '9') { return std::nullopt; }
            auto const digit = static_cast<unsigned>(byte - '0');
            if (number > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
                number = std::numeric_limits<std::uint64_t>::max();
            } else { number = number * 10 + digit; }
        }
        auto const nonzero = text.find_first_not_of('0');
        text.remove_prefix(nonzero == text.npos ? text.size() - 1 : nonzero);
        return std::pair{ number, text };
    }

    /**
     * @brief Resolve bounded byte ranges, ignoring unsupported units and malformed syntax.
     * @param text Entire Range value, already bounded by request header admission.
     * @param size Selected regular file size; empty representations ignore Range.
     * @param maximum Positive maximum specifier count.
     * @return Ordered satisfiable intervals or explicit full/416 policy.
     */
    auto static_ranges(
        std::string_view text,
        std::uint64_t size,
        std::size_t maximum
    ) -> StaticRanges {
        if (text.size() < 6 || !utils::iequals(text.substr(0, 6), "bytes=") || size == 0) { return {}; }
        text.remove_prefix(6);
        StaticRanges result{ .applied = true };
        std::size_t count{};
        for (;;) {
            if (++count > maximum) { return { .applied = true, .rejected = true }; }
            auto const comma = text.find(',');
            auto item = utils::trim_ows(text.substr(0, comma));
            auto const dash = item.find('-');
            if (dash == item.npos) { return {}; }
            auto left = item.substr(0, dash), right = item.substr(dash + 1);
            auto first = static_decimal(left), last = static_decimal(right);
            if (left.empty()) {
                if (!last) { return {}; }
                if (last->first != 0) { result.values.push_back({ size - std::min(size, last->first), size - 1 }); }
            } else {
                if (!first || (!right.empty() && !last)) { return {}; }
                if (last && (first->second.size() > last->second.size()
                    || (first->second.size() == last->second.size() && first->second > last->second))) { return {}; }
                if (first->first < size) { result.values.push_back({ first->first, last ? std::min(last->first, size - 1) : size - 1 }); }
            }
            if (comma == text.npos) { break; }
            text.remove_prefix(comma + 1);
        }
        result.rejected = result.values.empty();
        return result;
    }

    /**
     * @brief Weakly compare a complete If-None-Match list, preserving commas inside opaque entity tags.
     * @param text Entire combined field value.
     * @param tag Current quoted weak validator.
     * @return Whether a syntactically valid tag list or wildcard matches the current representation.
     */
    auto static_etag_matches(std::string_view text, std::string_view tag) noexcept -> bool {
        text = utils::trim_ows(text);
        if (text == "*") { return true; }
        if (tag.starts_with("W/")) { tag.remove_prefix(2); }
        bool matched{}, any{};
        for (;;) {
            text = utils::trim_ows(text);
            if (text.starts_with("W/")) { text.remove_prefix(2); }
            if (!text.starts_with('"')) { return false; }
            auto const end = text.find('"', 1);
            if (end == text.npos) { return false; }
            auto const candidate = text.substr(0, end + 1);
            for (unsigned char byte : candidate.substr(1, candidate.size() - 2)) {
                if (byte < 0x21 || byte == 0x7f) { return false; }
            }
            matched = matched || candidate == tag;
            any = true;
            text = utils::trim_ows(text.substr(end + 1));
            if (text.empty()) { return any && matched; }
            if (!text.starts_with(',')) { return false; }
            text.remove_prefix(1);
        }
    }

} // namespace mhttp::impl
