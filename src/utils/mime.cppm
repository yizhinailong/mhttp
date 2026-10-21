/**
 * @file mime.cppm
 * @brief Look up common media types by a filename's final extension using ASCII case folding.
 */
export module mhttp:utils.mime;

import std;

export namespace mhttp {

    /**
     * @brief Resolve a common filename extension without allocation or filesystem access.
     * @param filename Filename or slash-separated path; query strings are not stripped.
     * @param fallback Borrowed media type for missing or unknown extensions.
     * @return Static media type, or fallback whose lifetime remains the caller's responsibility.
     */
    [[nodiscard]] auto MimeType(
        std::string_view filename,
        std::string_view fallback = "application/octet-stream"
    ) noexcept -> std::string_view {
        constexpr std::pair<std::string_view, std::string_view> TYPES[]{
            { "html", "text/html; charset=utf-8" }, { "htm", "text/html; charset=utf-8" },
            { "txt", "text/plain; charset=utf-8" }, { "css", "text/css; charset=utf-8" },
            { "csv", "text/csv; charset=utf-8" }, { "js", "text/javascript" }, { "mjs", "text/javascript" },
            { "json", "application/json" }, { "map", "application/json" }, { "xml", "application/xml" },
            { "pdf", "application/pdf" }, { "zip", "application/zip" }, { "gz", "application/gzip" },
            { "tar", "application/x-tar" }, { "7z", "application/x-7z-compressed" },
            { "wasm", "application/wasm" }, { "bin", "application/octet-stream" },
            { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" },
            { "gif", "image/gif" }, { "webp", "image/webp" }, { "avif", "image/avif" },
            { "svg", "image/svg+xml" }, { "ico", "image/vnd.microsoft.icon" }, { "bmp", "image/bmp" },
            { "tif", "image/tiff" }, { "tiff", "image/tiff" },
            { "mp3", "audio/mpeg" }, { "wav", "audio/wav" }, { "ogg", "audio/ogg" },
            { "flac", "audio/flac" }, { "aac", "audio/aac" }, { "opus", "audio/ogg" },
            { "mp4", "video/mp4" }, { "webm", "video/webm" }, { "mpeg", "video/mpeg" },
            { "woff", "font/woff" }, { "woff2", "font/woff2" }, { "ttf", "font/ttf" }, { "otf", "font/otf" }
        };
        auto const slash = filename.find_last_of("/\\");
        auto const dot = filename.find_last_of('.');
        if (dot == filename.npos || (slash != filename.npos && dot <= slash)) { return fallback; }
        auto const extension = filename.substr(dot + 1);
        for (auto const& [key, value] : TYPES) {
            if (extension.size() != key.size()) { continue; }
            bool matches{ true };
            for (std::size_t i{}; i != key.size(); ++i) {
                auto byte = extension[i];
                if (byte >= 'A' && byte <= 'Z') { byte = static_cast<char>(byte + ('a' - 'A')); }
                if (byte != key[i]) { matches = false; break; }
            }
            if (matches) { return value; }
        }
        return fallback;
    }

} // namespace mhttp
