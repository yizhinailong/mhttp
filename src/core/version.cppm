/**
 * @file version.cppm
 * @brief Identify the library release used by protocol-facing defaults.
 */
export module mhttp:core.version;

import std;

export namespace mhttp {

    inline constexpr std::string_view VERSION{ "0.1.0" }; ///< Library release, matching the package manifest.

} // namespace mhttp
