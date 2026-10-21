/**
 * @file static_files.cppm
 * @brief Configure bounded static representations independently of filesystem and transport backends.
 */
export module mhttp:server.static_files;

import std;

export namespace mhttp::server {

    /**
     * @brief Configure one static mount before the router is frozen.
     * @note File selection and reading run on Runtime workers. Every path component below the root must be a
     *       regular file or directory; symbolic links and Windows reparse points are rejected.
     */
    struct StaticFileOptions {
        std::string index_file{ "index.html" }; ///< Single filename for directories, or empty to disable indexes.
        std::size_t max_ranges{ 16 }; ///< Maximum requested byte ranges, in [1, 128]; excess requests receive 416.
        std::vector<std::pair<std::string, std::string>> mime_types; ///< Extension overrides without a leading dot.
    };

} // namespace mhttp::server
