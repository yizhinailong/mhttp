/**
 * @file mount_point.cppm
 * @brief Serve a directory tree under a URL prefix, with optional HTML or JSON listings.
 */
export module mhttp:mount_point;

import std;
import nlohmann.json;
import :config;
import :strings;
import :http_date;
import :request;
import :response;

namespace mhttp::detail {

    /**
     * @brief View a filesystem name as UTF-8 bytes.
     * @param name UTF-8 string.
     * @return Byte view.
     */
    auto u8_view(std::u8string const& name) -> std::string_view {
        return std::string_view{ reinterpret_cast<char const*>(name.data()), name.size() };
    }

    /**
     * @brief Format a byte count like "  1.234kB".
     * @param bytes Size in bytes.
     * @return Size with a binary unit suffix.
     */
    auto format_size(double bytes) -> std::string {
        static constexpr std::array<std::string_view, 5> UNITS{ "kB", "MB", "GB", "TB", "PB" };
        for (auto const unit : UNITS) {
            bytes /= 1024.0;
            if (std::fabs(bytes) < 1024.0 || unit == UNITS.back()) {
                return std::format("{:4.3f}{}", bytes, unit);
            }
        }
        return {};
    }

    /**
     * @brief Render a directory listing as HTML (directories first).
     * @param target Request path shown in the title.
     * @param path Directory to list.
     * @param include_parent Emit a "../" row; false at the mount root.
     * @param ec Receives directory iteration errors.
     * @return HTML page.
     */
    auto format_dir_to_html(
        std::string_view target,
        std::filesystem::path const& path,
        bool include_parent,
        std::error_code& ec
    ) -> std::string {
        static constexpr std::size_t NAME_WIDTH{ 50 };
        std::vector<std::string> directories;
        std::vector<std::string> files;
        for (auto const& entry : std::filesystem::directory_iterator{ path, ec }) {
            std::error_code entry_ec;
            bool const is_directory{ entry.is_directory(entry_ec) };
            auto name{ entry.path().filename().u8string() };
            if (is_directory) {
                name += u8"/";
            }
            auto const time{ format_listing_time(file_last_write_time(entry.path(), entry_ec)) };
            auto shown{ html_escape(u8_view(name)) };
            if (name.size() > NAME_WIDTH) {
                shown = html_escape(u8_view(name).substr(0, NAME_WIDTH - 3)) + "..&gt;";
            }
            auto const padding{ std::string(NAME_WIDTH > name.size() ? NAME_WIDTH - name.size() : 0, ' ') };
            std::string size{ "-" };
            if (!is_directory) {
                auto const bytes{ entry.file_size(entry_ec) };
                size = format_size(entry_ec ? 0.0 : static_cast<double>(bytes));
            }
            auto row{ std::format("<a href=\"{}\">{}</a>{} {}       {}\r\n", href_encode(u8_view(name)), shown, padding, time, size) };
            (is_directory ? directories : files).push_back(std::move(row));
        }
        if (ec) {
            return {};
        }
        auto const title{ html_escape(target) };
        std::string body{ std::format("<html><head><meta charset=\"UTF-8\"><title>Index of {0}</title></head><body bgcolor=\"white\"><h1>Index of {0}</h1><hr><pre>", title) };
        if (include_parent) {
            body += "<a href=\"../\">../</a>\r\n";
        }
        for (auto const& row : directories) {
            body += row;
        }
        for (auto const& row : files) {
            body += row;
        }
        body += "</pre><hr></body></html>";
        return body;
    }

    /**
     * @brief Render a directory listing as a JSON array.
     * @param path Directory to list.
     * @param ec Receives directory iteration errors.
     * @return Array of {filename, is_dir, filesize, last_write_time}.
     */
    auto format_dir_to_json(std::filesystem::path const& path, std::error_code& ec) -> Json {
        auto list = Json::array(); // Not `list{ ... }`: that would nest the array in another one.
        for (auto const& entry : std::filesystem::directory_iterator{ path, ec }) {
            std::error_code entry_ec;
            bool const is_directory{ entry.is_directory(entry_ec) };
            // httplib reported the listed directory's time for every entry.
            list.push_back(Json{
                { "filename", std::string{ u8_view(entry.path().filename().u8string()) } },
                { "is_dir", is_directory },
                { "filesize", is_directory ? 0 : entry.file_size(entry_ec) },
                { "last_write_time", file_last_write_time(entry.path(), entry_ec) },
            });
        }
        return list;
    }

    /**
     * @brief Split a mount point into literal segments, ignoring leading and trailing '/'.
     * @param mount_point Mount point such as "/static/".
     * @return Segments; empty for the root.
     */
    auto split_mount_point(std::string_view mount_point) -> std::vector<std::string> {
        while (mount_point.starts_with('/')) {
            mount_point.remove_prefix(1);
        }
        while (mount_point.ends_with('/')) {
            mount_point.remove_suffix(1);
        }
        std::vector<std::string> out;
        if (mount_point.empty()) {
            return out;
        }
        for (auto const segment : split_path_segments(mount_point)) {
            out.emplace_back(segment);
        }
        return out;
    }

    /**
     * @brief Report whether `path` resolves inside `base_dir`.
     * @param path Candidate path.
     * @param base_dir Mount directory.
     * @param ec Receives canonicalization errors.
     * @return True when the canonical path is the base or below it.
     */
    auto is_within(
        std::filesystem::path const& path,
        std::filesystem::path const& base_dir,
        std::error_code& ec
    ) -> bool {
        auto const base{ std::filesystem::weakly_canonical(base_dir, ec).string() };
        if (ec) {
            return false;
        }
        auto const resolved{ std::filesystem::weakly_canonical(path, ec).string() };
        if (ec || resolved.size() < base.size() || !resolved.starts_with(base)) {
            return false;
        }
        if (resolved.size() == base.size()) {
            return true;
        }
        // A root with a trailing separator (e.g. "C:\") contains everything that starts with it.
        return resolved[base.size()] == std::filesystem::path::preferred_separator || base.back() == std::filesystem::path::preferred_separator;
    }

} // namespace mhttp::detail

export namespace mhttp {

    /**
     * @brief Static directory served under a URL prefix (see Router::SetStaticMountPoint()).
     */
    class MountPointEntry {
    public:
        /**
         * @brief Format of generated directory listings.
         */
        enum class DirFormat {
            Json, ///< JSON array of entries.
            Html, ///< nginx-style HTML index.
        };

    private:
        std::string m_mount_point;                                            ///< URL prefix as registered.
        std::filesystem::path m_base_dir;                                     ///< Directory being served.
        std::vector<std::string> m_mount_segments;                            ///< Decoded prefix segments.
        bool m_enabled_directory{ true };                                     ///< Generate listings.
        DirFormat m_directory_format{ DirFormat::Json };                      ///< Listing format.
        std::vector<std::string> m_default_documents{ "index.html", "index.htm" }; ///< Documents tried for directories.

    public:
        /**
         * @brief Serve `base_dir` under `mount_point`.
         * @param mount_point URL prefix, e.g. "/static"; decoded text, no %XX escapes.
         * @param base_dir Directory to serve.
         */
        MountPointEntry(std::string mount_point, std::filesystem::path base_dir)
            : m_mount_point{ std::move(mount_point) }, m_base_dir{ std::move(base_dir) }, m_mount_segments{ detail::split_mount_point(m_mount_point) } {}

        [[nodiscard]] auto MountPoint() const noexcept -> std::string const& {
            return m_mount_point;
        }

        [[nodiscard]] auto BaseDir() const noexcept -> std::filesystem::path const& {
            return m_base_dir;
        }

        auto SetEnabledDirectory(bool enabled) -> void {
            m_enabled_directory = enabled;
        }

        auto SetDirectoryFormat(DirFormat format) -> void {
            m_directory_format = format;
        }

        auto SetDefaultDocumentName(std::vector<std::string> names) -> void {
            m_default_documents = std::move(names);
        }

        /**
         * @brief Serve one request.
         *
         * - A directory without a trailing '/' redirects (301) to add it, unless listings are
         *   off and no default document exists (404, so the directory's existence stays hidden).
         * - A trailing '/' only matches directories; a file with one is 404.
         * - Segments containing '/', '\\', or NUL (from %2F etc.) and ".." above the mount are 400.
         *
         * @param req Request; its decoded segments select the file.
         * @param resp Response to fill in.
         */
        auto operator()(Request& req, Response& resp) const -> void {
            auto const segments{ req.DecodedSegments() };
            if (segments.size() < m_mount_segments.size() || !std::ranges::equal(segments.first(m_mount_segments.size()), m_mount_segments)) {
                resp.SetErrorContent(Status::not_found);
                return;
            }
            auto const relative{ segments.subspan(m_mount_segments.size()) };
            bool const dir_requested{ !relative.empty() && relative.back().empty() };

            std::size_t level{ 0 };
            std::filesystem::path relative_path;
            for (auto const& segment : relative) {
                if (segment.find_first_of(std::string_view{ "/\\\0", 3 }) != std::string::npos) {
                    resp.SetErrorContent(Status::bad_request);
                    return;
                }
                if (segment.empty() || segment == ".") {
                    continue;
                }
                if (segment == "..") {
                    if (level == 0) {
                        resp.SetErrorContent(Status::bad_request);
                        return;
                    }
                    --level;
                } else {
                    ++level;
                }
                relative_path /= std::filesystem::path{ std::u8string_view{ reinterpret_cast<char8_t const*>(segment.data()), segment.size() } };
            }

            std::error_code ec;
            auto const path{ m_base_dir / relative_path };
            if (!detail::is_within(path, m_base_dir, ec)) {
                resp.SetErrorContent(Status::forbidden);
                return;
            }
            if (!std::filesystem::exists(path, ec)) {
                resp.SetErrorContent(Status::not_found);
                return;
            }

            if (!dir_requested) {
                if (std::filesystem::is_regular_file(path, ec)) {
                    resp.SetFileContent(path, req.Base());
                    return;
                }
                if (std::filesystem::is_directory(path, ec)) {
                    if (!m_enabled_directory && !hasDefaultDocument(path)) {
                        resp.SetErrorContent(Status::not_found);
                        return;
                    }
                    std::string location{ req.Path() };
                    location += '/';
                    auto const target{ req.Target() };
                    if (auto const question{ target.find('?') }; question != std::string_view::npos) {
                        location.append(target.substr(question));
                    }
                    resp.SetRedirect(location, Status::moved_permanently);
                    return;
                }
                resp.SetErrorContent(Status::not_found);
                return;
            }

            if (!std::filesystem::is_directory(path, ec)) {
                resp.SetErrorContent(Status::not_found);
                return;
            }
            for (auto const& name : m_default_documents) {
                if (auto const document{ path / name }; std::filesystem::is_regular_file(document, ec)) {
                    resp.SetFileContent(document, req.Base());
                    return;
                }
            }
            if (!m_enabled_directory) {
                resp.SetErrorContent(Status::forbidden);
                return;
            }

            std::error_code list_ec;
            if (m_directory_format == DirFormat::Json) {
                // `auto x{ json }` would pick nlohmann's initializer_list constructor and wrap the
                // listing in a one-element array.
                auto const listing = detail::format_dir_to_json(path, list_ec);
                if (!list_ec) {
                    resp.SetJsonContent(listing);
                }
            } else {
                // The mount root has no parent inside the mount; "../" would leave it.
                auto listing{ detail::format_dir_to_html(req.Path(), path, !relative_path.empty(), list_ec) };
                if (!list_ec) {
                    resp.SetStringContent(std::move(listing), "text/html; charset=utf-8");
                }
            }
            if (list_ec) {
                resp.SetErrorContent(Status::internal_server_error);
            }
        }

    private:
        /**
         * @brief Report whether a directory holds one of the default documents.
         * @param dir Directory to check.
         * @return True when one exists.
         */
        [[nodiscard]] auto hasDefaultDocument(std::filesystem::path const& dir) const -> bool {
            std::error_code ec;
            return std::ranges::any_of(m_default_documents, [&](auto const& name) { return std::filesystem::is_regular_file(dir / name, ec); });
        }
    };

} // namespace mhttp
