/**
 * @file static_response.cppm
 * @brief Select conditional file representations on workers and stream exact bounded single or multipart ranges.
 */
module mhttp:server.impl.static_response;

import std;
import :server.impl.static_file;
import :server.impl.static_policy;
import :server.static_files;
import :server.handler;
import :core.runtime;
import :utils.mime;
import :utils.date;
import :utils.string;

namespace mhttp::impl {

    /**
     * @brief Own a complete file response plan before committing any response headers.
     */
    struct StaticPlan {
        Status status{ Status::Ok }; ///< Selected full, partial, conditional, or error response.
        std::shared_ptr<StaticFile> file; ///< Stable native file retained through the complete provider.
        std::string mime; ///< Owned full or multipart response media type.
        std::string etag; ///< Weak metadata validator, never treated as a strong If-Range validator.
        std::string modified; ///< Last-Modified, bounded above by response selection time.
        std::string content_range; ///< Single-range or unsatisfiable response field.
        std::vector<StaticRange> ranges; ///< Selected intervals in request order.
        std::vector<std::string> part_heads; ///< Owned bounded multipart preambles, empty for a single range.
        std::string terminal; ///< Multipart closing boundary, or empty for ordinary output.
        std::uint64_t length{}; ///< Exact sum of selected bytes and multipart framing.
    };

    /**
     * @brief Own immutable mount configuration separately from its callback to avoid ownership cycles.
     */
    struct StaticMountData {
        std::shared_ptr<StaticRoot> root; ///< Pinned configured directory.
        server::StaticFileOptions options; ///< Validated index, MIME overrides, and range bound.
        std::vector<std::string> prefix; ///< Decoded portable registration components.
        std::string key; ///< Canonical decoded prefix for replacement and removal.
    };

    /**
     * @brief Select file metadata, conditional status, and exact framing on a worker thread.
     * @param mount Owned immutable configuration.
     * @param relative Owned decoded components below the mount.
     * @param headers Owned request fields, bounded by server admission.
     * @param head Whether this is HEAD, which ignores Range.
     * @return Owned response plan; missing/unsafe native objects remain 404.
     */
    auto prepare_static_file(
        StaticMountData const& mount,
        std::vector<std::string> relative,
        Headers const& headers,
        bool head
    ) -> StaticPlan {
        StaticPlan plan;
        try {
            auto opened = mount.root->Open(std::move(relative), mount.options.index_file);
            if (!opened) { plan.status = opened.error(); return plan; }
            plan.file = std::move(*opened);
        } catch (std::filesystem::filesystem_error const&) {
            plan.status = Status::NotFound;
            return plan;
        }
        auto const& metadata = plan.file->Metadata();
        auto const now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto const modified = std::min(metadata.modified, now);
        plan.modified = FormatHttpDate(modified);
        plan.etag = std::format("W/\"{}\"", metadata.identity);
        plan.mime = MimeType(plan.file->Name());
        auto const dot = plan.file->Name().find_last_of('.');
        if (dot != std::string_view::npos) {
            for (auto const& [extension, mime] : mount.options.mime_types | std::views::reverse) {
                if (utils::iequals(extension, plan.file->Name().substr(dot + 1))) { plan.mime = mime; break; }
            }
        }
        if (headers.Has("If-Match") && (headers.Count("If-Match") != 1 || utils::trim_ows(headers.Get("If-Match")) != "*")) {
            plan.status = Status::PreconditionFailed;
            return plan;
        }
        if (!headers.Has("If-Match") && headers.Count("If-Unmodified-Since") == 1) {
            auto date = ParseHttpDate(utils::trim_ows(headers.Get("If-Unmodified-Since")));
            if (date && modified > *date) { plan.status = Status::PreconditionFailed; return plan; }
        }
        if (headers.Has("If-None-Match")) {
            std::string tags;
            for (auto value : headers.Values("If-None-Match")) {
                if (!tags.empty()) { tags += ','; }
                tags += value;
            }
            if (static_etag_matches(tags, plan.etag)) { plan.status = Status::NotModified; return plan; }
        } else if (headers.Count("If-Modified-Since") == 1) {
            auto date = ParseHttpDate(utils::trim_ows(headers.Get("If-Modified-Since")));
            if (date && modified <= *date) { plan.status = Status::NotModified; return plan; }
        }
        StaticRanges ranges;
        // Metadata validators cannot establish strong If-Range equality, including second-resolution dates.
        if (!head && headers.Count("Range") == 1 && !headers.Has("If-Range")) {
            ranges = static_ranges(utils::trim_ows(headers.Get("Range")), metadata.size, mount.options.max_ranges);
        }
        if (ranges.rejected) {
            plan.status = Status::RangeNotSatisfiable;
            plan.content_range = std::format("bytes */{}", metadata.size);
            return plan;
        }
        if (!ranges.applied) {
            plan.length = metadata.size;
            if (metadata.size != 0) { plan.ranges.push_back({ 0, metadata.size - 1 }); }
            return plan;
        }
        plan.status = Status::PartialContent;
        plan.ranges = std::move(ranges.values);
        if (plan.ranges.size() == 1) {
            auto const range = plan.ranges.front();
            plan.content_range = std::format("bytes {}-{}/{}", range.first, range.last, metadata.size);
            plan.length = range.last - range.first + 1;
            return plan;
        }
        std::random_device random;
        auto boundary = std::format("mhttp-{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
        plan.terminal = std::format("--{}--\r\n", boundary);
        plan.length = plan.terminal.size();
        for (auto const range : plan.ranges) {
            auto preamble = std::format("--{}\r\nContent-Type: {}\r\nContent-Range: bytes {}-{}/{}\r\n\r\n",
                boundary, plan.mime, range.first, range.last, metadata.size);
            auto const data = range.last - range.first + 1;
            if (preamble.size() + 2 > std::numeric_limits<std::uint64_t>::max() - plan.length
                || data > std::numeric_limits<std::uint64_t>::max() - plan.length - preamble.size() - 2) {
                plan.status = Status::RangeNotSatisfiable;
                plan.content_range = std::format("bytes */{}", metadata.size);
                return plan;
            }
            plan.length += preamble.size() + data + 2;
            plan.part_heads.push_back(std::move(preamble));
        }
        plan.mime = std::format("multipart/byteranges; boundary={}", boundary);
        return plan;
    }

    /**
     * @brief Split small owned multipart framing to fit even very small stream segment limits.
     * @param writer Borrowed active provider window.
     * @param bytes Owned framing retained until all bounded writes complete.
     * @return Completion or first sticky stream failure.
     */
    auto write_static_text(server::StreamWriter& writer, std::string bytes) -> Task<Result<void>> {
        auto const bound = writer.MaxWriteSize();
        if (bound == 0) { co_return std::unexpected{ Error::BodyTooLarge }; }
        for (std::size_t offset{}; offset < bytes.size();) {
            auto const count = std::min(bound, bytes.size() - offset);
            auto written = co_await writer.Write(bytes.substr(offset, count));
            if (!written) { co_return written; }
            offset += count;
        }
        co_return Result<void>{};
    }

    /**
     * @brief Await worker reads and transport writes sequentially without aggregate file buffering.
     * @param plan Owned immutable plan retained by the deferred provider.
     * @param writer Borrowed stream, valid through the complete provider Task.
     * @return Completion or read, cancellation, timeout, or transport failure.
     */
    auto stream_static_file(
        std::shared_ptr<StaticPlan const> plan,
        server::StreamWriter& writer
    ) -> Task<Result<void>> {
        auto const bound = writer.MaxWriteSize();
        if (bound == 0) { co_return std::unexpected{ Error::BodyTooLarge }; }
        for (std::size_t i{}; i < plan->ranges.size(); ++i) {
            if (!plan->part_heads.empty()) {
                auto written = co_await write_static_text(writer, plan->part_heads[i]);
                if (!written) { co_return written; }
            }
            auto offset = plan->ranges[i].first;
            auto remaining = plan->ranges[i].last - offset + 1;
            while (remaining != 0) {
                auto const count = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, std::min<std::size_t>(bound, 65536)));
                auto bytes = co_await Offload([file = plan->file, offset, count] { return file->Read(offset, count); });
                if (!bytes) { co_return std::unexpected{ bytes.error() }; }
                auto written = co_await writer.Write(std::move(*bytes));
                if (!written) { co_return written; }
                offset += count;
                remaining -= count;
            }
            if (!plan->part_heads.empty()) {
                auto written = co_await write_static_text(writer, "\r\n");
                if (!written) { co_return written; }
            }
        }
        co_return co_await write_static_text(writer, plan->terminal);
    }

    /**
     * @brief Execute a selected mount after global Before aspects and defer bytes until final hooks.
     * @param mount Owned immutable mount state, independent of the public Router wrapper.
     * @param request Borrowed valid request through the handler and provider lifetimes.
     * @param response Borrowed response finalized by the normal routing chain.
     */
    auto serve_static_file(
        std::shared_ptr<StaticMountData const> mount,
        server::Request& request,
        server::Response& response
    ) -> Task<void> {
        if (request.Method() != Method::Get && request.Method() != Method::Head) {
            static_cast<void>(response.SetError(Status::MethodNotAllowed));
            static_cast<void>(response.Headers().Set("Allow", "GET, HEAD"));
            co_return;
        }
        auto components = static_components(detail::ServerRequestAccess::EncodedPath(request));
        if (!components || components->size() < mount->prefix.size()) {
            static_cast<void>(response.SetError(Status::NotFound));
            co_return;
        }
        components->erase(components->begin(), components->begin() + static_cast<std::ptrdiff_t>(mount->prefix.size()));
        auto plan = co_await Offload([mount, relative = std::move(*components), headers = request.Headers(),
            head = request.Method() == Method::Head] { return prepare_static_file(*mount, relative, headers, head); });
        if (!plan.etag.empty()) {
            static_cast<void>(response.Headers().Set("ETag", plan.etag));
            static_cast<void>(response.Headers().Set("Last-Modified", plan.modified));
            static_cast<void>(response.Headers().Set("Accept-Ranges", "bytes"));
        }
        if (!plan.content_range.empty()) { static_cast<void>(response.Headers().Set("Content-Range", plan.content_range)); }
        if (plan.status == Status::NotModified) { static_cast<void>(response.SetEmpty(plan.status)); }
        else if (IsClientError(plan.status) || IsServerError(plan.status)) { static_cast<void>(response.SetError(plan.status)); }
        else {
            static_cast<void>(response.SetStatus(plan.status));
            auto owned = std::make_shared<StaticPlan const>(std::move(plan));
            static_cast<void>(response.SetContentProvider(owned->length,
                [owned](auto& writer) { return stream_static_file(owned, writer); }, owned->mime));
        }
    }

    /**
     * @brief Own a mount callback and immutable path selection data without cyclic shared ownership.
     */
    struct StaticMount {
        std::shared_ptr<StaticMountData const> data; ///< Validated immutable mount and native root.
        std::shared_ptr<detail::HttpHandler const> handler; ///< Stable callback alias for prepared routing.

        /**
         * @brief Validate portable configuration and open a root on the caller's non-I/O thread.
         * @param prefix Decoded absolute mount prefix; one trailing slash is canonicalized away.
         * @param root Borrowed native directory name copied/opened during this call.
         * @param options Owned bounded options and validated MIME overrides.
         * @return Owned mount, InvalidRequest for invalid configuration, or Busy on an I/O thread.
         */
        static auto Create(
            std::string_view prefix,
            std::filesystem::path const& root,
            server::StaticFileOptions options
        ) -> Result<std::shared_ptr<StaticMount const>> {
            if (Runtime::IsIoThread()) { return std::unexpected{ Error::Busy }; }
            auto parts = static_components(prefix, false);
            if (!parts || options.max_ranges == 0 || options.max_ranges > 128
                || (!options.index_file.empty() && !static_component(options.index_file))) {
                return std::unexpected{ Error::InvalidRequest };
            }
            for (auto const& [extension, mime] : options.mime_types) {
                if (extension.empty() || !utils::is_field_value(mime) || mime.empty()) { return std::unexpected{ Error::InvalidHeaders }; }
                for (auto byte : extension) {
                    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
                        || byte == '-' || byte == '_')) { return std::unexpected{ Error::InvalidRequest }; }
                }
            }
            auto directory = StaticRoot::Create(root);
            if (!directory) { return std::unexpected{ directory.error() }; }
            std::string key;
            for (auto const& part : *parts) { key += '/'; key += part; }
            if (key.empty()) { key = "/"; }
            auto data = std::make_shared<StaticMountData const>(StaticMountData{
                std::move(*directory), std::move(options), std::move(*parts), std::move(key) });
            auto handler = std::make_shared<detail::HttpHandler const>([data](auto& request, auto& response) {
                return serve_static_file(data, request, response);
            });
            return std::make_shared<StaticMount const>(StaticMount{ std::move(data), std::move(handler) });
        }
    };

} // namespace mhttp::impl
