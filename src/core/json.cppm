/**
 * @file json.cppm
 * @brief Adapt optional nlohmann JSON values to bounded, recoverable message readers and writers.
 */
export module mhttp.json;

import std;
export import nlohmann.json;
export import mhttp;

export namespace mhttp {

    using Json = nlohmann::json; ///< Optional owned JSON value, available with the json feature.
    using OrderedJson = nlohmann::ordered_json; ///< Optional JSON object preserving member insertion order.

} // namespace mhttp

namespace mhttp::detail {

    /**
     * @brief Check nesting outside strings before asking the JSON implementation to allocate a value tree.
     * @param body Borrowed JSON bytes, including any embedded NULs.
     * @param limits Positive nesting limit.
     * @return Success, InvalidRequest for invalid limits/unbalanced closing tokens, or BodyTooLarge for depth.
     */
    [[nodiscard]] auto validate_json_depth(std::string_view body, JsonLimits limits) -> Result<void> {
        if (limits.max_depth == 0 || body.contains('\0')) { return std::unexpected{ Error::InvalidRequest }; }
        std::size_t depth{};
        bool quoted{}, escaped{};
        for (char value : body) {
            if (quoted) {
                if (escaped) { escaped = false; }
                else if (value == '\\') { escaped = true; }
                else if (value == '"') { quoted = false; }
            } else if (value == '"') { quoted = true; }
            else if (value == '{' || value == '[') {
                if (++depth > limits.max_depth) { return std::unexpected{ Error::BodyTooLarge }; }
            } else if (value == '}' || value == ']') {
                if (depth == 0) { return std::unexpected{ Error::InvalidRequest }; }
                --depth;
            }
        }
        return {};
    }

} // namespace mhttp::detail

export namespace mhttp::detail {

    /**
     * @brief Activate the default message JSON API without changing core message definitions or their dependencies.
     * @internal
     */
    template <>
    struct JsonAdapter<void> {
        /**
         * @brief Identify supported owned JSON types, excluding implicit conversions from strings and scalars.
         * @tparam T Candidate representation.
         */
        template <typename T>
        static constexpr bool IS_VALUE = std::same_as<std::remove_cvref_t<T>, Json>
            || std::same_as<std::remove_cvref_t<T>, OrderedJson>;

        /**
         * @brief Parse a complete application/json or structured +json representation into an owned value.
         * @param body Borrowed complete bytes, never retained after return.
         * @param headers Borrowed media-type declaration, including optional parameters.
         * @param limits Nesting limit checked before allocation/parsing.
         * @return Owned JSON, InvalidHeaders for duplicate Content-Type, InvalidRequest for syntax/type/limit errors,
         *         or BodyTooLarge for excessive nesting; allocation failures propagate.
         */
        [[nodiscard]] static auto Parse(
            std::string_view body,
            Headers const& headers,
            JsonLimits limits
        ) -> Result<Json> {
            if (headers.Count("Content-Type") > 1) { return std::unexpected{ Error::InvalidHeaders }; }
            auto type = headers.Get("Content-Type");
            type = utils::trim_ows(type.substr(0, type.find(';')));
            auto const subtype = type.substr(type.find('/') == std::string_view::npos ? type.size() : type.find('/') + 1);
            if (!utils::iequals(type, "application/json") && !(utils::iequals(type.substr(0, 12), "application/")
                && utils::is_field_name(subtype) && subtype.size() > 5
                && utils::iequals(subtype.substr(subtype.size() - 5), "+json"))) {
                return std::unexpected{ Error::InvalidRequest };
            }
            auto valid = validate_json_depth(body, limits);
            if (!valid) { return std::unexpected{ valid.error() }; }
            auto parsed = Json::parse(body.begin(), body.end(), nullptr, false);
            if (parsed.is_discarded()) { return std::unexpected{ Error::InvalidRequest }; }
            return parsed;
        }

        /**
         * @brief Serialize a supported JSON value before mutating any message state.
         * @tparam T Exact Json or OrderedJson value type.
         * @param value Borrowed JSON tree; no views are retained after return.
         * @return Owned UTF-8 JSON text or InvalidRequest for invalid values such as malformed UTF-8 strings.
         */
        template <typename T>
            requires IS_VALUE<T>
        [[nodiscard]] static auto Serialize(T const& value) -> Result<std::string> {
            try { return value.dump(); }
            catch (Json::exception const&) { return std::unexpected{ Error::InvalidRequest }; }
        }
    };

} // namespace mhttp::detail
