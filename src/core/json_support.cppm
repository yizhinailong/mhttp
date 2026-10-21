/**
 * @file json_support.cppm
 * @brief Declare optional JSON customization without importing a JSON implementation into core message types.
 */
export module mhttp:core.json_support;

import std;
export import :core.headers;

export namespace mhttp {

    /**
     * @brief Bound nested arrays/objects before optional JSON parsing to protect later recursive operations.
     */
    struct JsonLimits {
        std::size_t max_depth{ 128 }; ///< Maximum nesting depth; must be positive, scalar roots have depth zero.
    };

} // namespace mhttp

export namespace mhttp::detail {

    /**
     * @brief Supply JSON parsing/serialization only when the optional JSON module is imported.
     * @tparam Tag Deferred customization tag; the JSON feature specializes void.
     * @internal
     */
    template <typename Tag = void>
    struct JsonAdapter;

    /**
     * @brief Enable JSON readers only when the selected adapter is complete and exposes bounded parsing.
     * @tparam Adapter Optional implementation adapter.
     * @internal
     */
    template <typename Adapter>
    concept JsonReader = requires(std::string_view body, Headers const& headers, JsonLimits limits) {
        Adapter::Parse(body, headers, limits);
    };

    /**
     * @brief Enable JSON writers only for exact adapter-supported value types, avoiding string overload ambiguity.
     * @tparam T Candidate JSON value type.
     * @tparam Adapter Optional implementation adapter.
     * @internal
     */
    template <typename T, typename Adapter>
    concept JsonValue = requires { requires Adapter::template IS_VALUE<T>; };

} // namespace mhttp::detail
