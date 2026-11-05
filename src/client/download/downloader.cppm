/**
 * @file downloader.cppm
 * @brief Provide Downloader: resumable file downloads with retries, redirects, and caching.
 */
module;

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:downloader;

import std;
import nlohmann.json;
import :config;
import :error;
import :strings;
import :url;
import :headers;
import :async_event;
import :client_impl;
import :client_request;
import :client_response;
import :client;
import :client_pool;
import :cache;

namespace mhttp::client {

    class DownloaderImpl;

} // namespace mhttp::client

export namespace mhttp::client {

    /**
     * @brief Download a URL to a file (port of httplib::client::downloader).
     *
     * The body goes to `<save_path>.part` and is renamed to `save_path` on success; a failed or
     * cancelled run keeps the part file so a later run (or a retry within the run) resumes it
     * with a Range request. Redirects, conditional requests (ETag / Last-Modified, per hop), and
     * an optional response Cache are handled internally. One run at a time per downloader;
     * share one HttpClientPool between several downloaders to download concurrently.
     */
    class Downloader {
    public:
        /**
         * @brief Progress of the current run.
         */
        struct ProgressInfo {
            std::uint64_t total_bytes{ 0 };         ///< Size of the whole file; 0 when unknown.
            std::uint64_t downloaded_bytes{ 0 };    ///< Bytes on disk, resumed ones included.
            std::uint64_t speed_bytes_per_sec{ 0 }; ///< Average speed since the attempt started.
            std::chrono::seconds eta{ 0 };          ///< Estimated time left.
        };

        using ProgressCallback = std::function<void(ProgressInfo const&)>;

        /**
         * @brief Phase of the current run.
         */
        enum class State {
            Idle,
            Connecting,
            Downloading,
            Paused,
            Completed,
            Failed,
            Cancelled,
        };

        using StateCallback = std::function<void(State, boost::system::error_code)>;

        /**
         * @brief Retry, timeout, and transfer settings.
         */
        struct Config {
            int max_retries{ 3 };                                                      ///< Extra attempts after a transient failure.
            std::chrono::steady_clock::duration timeout{ std::chrono::seconds{ 60 } }; ///< Per I/O step, i.e. the stall limit.
            std::chrono::milliseconds acquire_timeout{ std::chrono::seconds{ 30 } };   ///< Wait for a pooled connection.
            int max_redirects{ 5 };                                                    ///< Redirects to follow.
            bool resume{ true };                                                       ///< Continue an existing part file.
            std::uint64_t max_speed_bytes_per_sec{ 0 };                                ///< Download cap; 0 = none.
            std::chrono::milliseconds retry_backoff{ 200 };                            ///< Delay per attempt number before a retry.
        };

    private:
        std::shared_ptr<DownloaderImpl> m_impl; ///< Run state; null after a move.

    public:
        /**
         * @brief Create a downloader.
         * @param executor Executor for timers and the background Download().
         * @param pool Connection pool to borrow clients from.
         */
        Downloader(net::any_io_executor const& executor, std::shared_ptr<HttpClientPool> pool);
        Downloader(net::io_context& context, std::shared_ptr<HttpClientPool> pool);

        /**
         * @brief Cancel a run in progress.
         */
        ~Downloader();

        Downloader(Downloader const&) = delete;
        auto operator=(Downloader const&) -> Downloader& = delete;
        Downloader(Downloader&& other) noexcept = default;
        auto operator=(Downloader&& other) noexcept -> Downloader&;

        auto SetConfig(Config const& config) -> void;
        [[nodiscard]] auto GetConfig() const -> Config;
        auto SetProgressCallback(ProgressCallback callback) -> void;
        auto SetStateCallback(StateCallback callback) -> void;

        /**
         * @brief Enable response caching.
         * @param cache Cache to use; nullptr disables caching.
         */
        auto SetCache(std::shared_ptr<Cache> cache) -> void;
        [[nodiscard]] auto GetCache() const -> std::shared_ptr<Cache>;
        [[nodiscard]] auto GetPool() const -> std::shared_ptr<HttpClientPool>;

        /**
         * @brief Download `url` into `save_path`.
         *
         * 408, 429, 5xx, connection failures, and interrupted bodies are retried up to
         * Config::max_retries times, resuming from the part file. Other non-success statuses
         * fail at once with Error::HttpStatus (see HttpStatus()).
         *
         * @param url Absolute http(s) URL.
         * @param save_path Destination file.
         * @param headers Extra request fields.
         * @return Error code; operation_aborted after Cancel(), Error::AlreadyInUse while
         *         another run is active (httplib did not guard this).
         */
        auto AsyncDownload(
            std::string_view url,
            std::filesystem::path const& save_path,
            Headers const& headers = {}
        ) -> net::awaitable<boost::system::error_code>;

        /**
         * @brief Run AsyncDownload() in the background.
         * @return Future of the result.
         */
        auto Download(
            std::string_view url,
            std::filesystem::path const& save_path,
            Headers const& headers = {}
        ) -> std::future<boost::system::error_code>;

        /**
         * @brief Stop the current run; in-flight I/O is aborted by closing its connection.
         * @note A run waiting for a pooled connection notices it once one is lent.
         */
        auto Cancel() -> void;
        auto Pause() -> void;
        auto Resume() -> void;
        [[nodiscard]] auto IsPaused() const -> bool;

        /**
         * @brief Return the file name from the last response's Content-Disposition.
         * @return Base name without directories (httplib kept "../" parts), or empty.
         */
        [[nodiscard]] auto SuggestedFilename() const -> std::string;
        [[nodiscard]] auto CurrentState() const -> State;

        /**
         * @brief Return the last response status of the current or most recent run.
         * @return Status, or Status::unknown when no response arrived.
         */
        [[nodiscard]] auto HttpStatus() const -> Status;
    };

} // namespace mhttp::client

namespace mhttp::detail {

    /**
     * @brief Report whether a comma-separated header holds a token (directive names only).
     * @param value Header value, e.g. Cache-Control.
     * @param token Token to look for, case-insensitively.
     * @return True when present.
     */
    auto header_has_token(std::string_view value, std::string_view token) -> bool {
        return std::ranges::any_of(split(value, ","), [&](std::string_view part) {
            return iequals(trim(part.substr(0, part.find('='))), token);
        });
    }

    /**
     * @brief Return an integer directive such as "max-age=60".
     * @param value Header value.
     * @param name Directive name.
     * @return Value, or std::nullopt when absent or malformed.
     */
    auto header_directive_int(std::string_view value, std::string_view name) -> std::optional<std::int64_t> {
        for (auto const part : split(value, ",")) {
            auto const equals{ part.find('=') };
            if (equals == std::string_view::npos || !iequals(trim(part.substr(0, equals)), name)) {
                continue;
            }
            auto text{ trim(part.substr(equals + 1)) };
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                text = text.substr(1, text.size() - 2);
            }
            std::int64_t result{ 0 };
            auto const [end, ec]{ std::from_chars(text.data(), text.data() + text.size(), result) };
            if (ec != std::errc{} || end != text.data() + text.size()) {
                return std::nullopt;
            }
            return result;
        }
        return std::nullopt;
    }

    /**
     * @brief Parsed "Content-Range: bytes first-last/total".
     */
    struct ContentRange {
        std::uint64_t start{ 0 }; ///< First byte offset.
        std::uint64_t end{ 0 };   ///< Last byte offset, inclusive.
        std::uint64_t total{ 0 }; ///< Complete length; 0 when unknown ("*").
    };

    /**
     * @brief Validate every component of a satisfied byte Content-Range.
     * @param value Response field.
     * @return Parsed range, or no value when malformed or inconsistent.
     */
    auto parse_content_range(std::string_view value) -> std::optional<ContentRange> {
        if (!value.starts_with("bytes ")) {
            return std::nullopt;
        }
        value.remove_prefix(6);
        auto const parse{ [](std::string_view text) -> std::optional<std::uint64_t> {
            std::uint64_t result{ 0 };
            auto const [end, ec]{ std::from_chars(text.data(), text.data() + text.size(), result) };
            return ec == std::errc{} && end == text.data() + text.size() && !text.empty() ? std::optional{ result } : std::nullopt;
        } };
        auto const dash{ value.find('-') };
        auto const slash{ value.find('/') };
        if (dash == std::string_view::npos || slash == std::string_view::npos || dash >= slash) {
            return std::nullopt;
        }
        auto const start{ parse(value.substr(0, dash)) };
        auto const end{ parse(value.substr(dash + 1, slash - dash - 1)) };
        auto const total_text{ value.substr(slash + 1) };
        auto const total{ parse(total_text) };
        if (!start || !end || *end < *start || *end == (std::numeric_limits<std::uint64_t>::max)() || (total_text != "*" && (!total || *end >= *total))) {
            return std::nullopt;
        }
        return ContentRange{ *start, *end, total.value_or(0) };
    }

    /**
     * @brief Extract the file name of a Content-Disposition value.
     * @param value Header value; "filename*=charset''pct-encoded" wins over "filename=".
     * @return Base name with any directory part removed, or empty.
     */
    auto parse_content_disposition_filename(std::string_view value) -> std::string {
        std::string lower{ value };
        std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string name;
        if (auto const extended{ lower.find("filename*=") }; extended != std::string::npos) {
            auto const first_quote{ value.find('\'', extended + 10) };
            auto const second_quote{ first_quote == std::string_view::npos ? first_quote : value.find('\'', first_quote + 1) };
            if (second_quote != std::string_view::npos) {
                auto const rest{ value.substr(second_quote + 1) };
                name = url::url_decode(trim(rest.substr(0, rest.find(';'))));
            }
        }
        if (auto const plain{ lower.find("filename=") }; name.empty() && plain != std::string::npos) {
            auto rest{ value.substr(plain + 9) };
            if (rest.starts_with('"')) {
                rest.remove_prefix(1);
                name = std::string{ rest.substr(0, rest.find('"')) };
            } else {
                name = std::string{ trim(rest.substr(0, rest.find(';'))) };
            }
        }
        if (auto const slash{ name.find_last_of("/\\") }; slash != std::string::npos) {
            name.erase(0, slash + 1);
        }
        return name == "." || name == ".." ? std::string{} : name;
    }

    /**
     * @brief Derive a cache scope from credentials so entries never leak across them.
     * @param headers Request fields.
     * @return Hash of the credential fields, or empty without credentials.
     */
    auto cache_auth_scope(Headers const& headers) -> std::string {
        std::string material;
        for (auto const name : { "Authorization", "Proxy-Authorization", "Cookie", "Cookie2" }) {
            if (auto const value{ headers[std::string_view{ name }] }; !value.empty()) {
                material += std::format("{}:{}\n", name, value);
            }
        }
        return material.empty() ? std::string{} : fnv1a_hex(material);
    }

    /**
     * @brief Downloader bookkeeping kept in a cache entry's metadata blob.
     */
    struct HttpMeta {
        std::string etag;                                                 ///< Validator.
        std::string last_modified;                                        ///< Validator.
        std::string content_type;                                         ///< Informational.
        std::string content_disposition;                                  ///< Informational.
        std::string cache_control;                                        ///< Freshness policy retained across 304s.
        std::optional<std::chrono::system_clock::time_point> fresh_until; ///< From max-age.
        bool must_revalidate{ false };                                    ///< Cache-Control: no-cache.
    };

    auto make_meta(Headers const& response) -> HttpMeta {
        HttpMeta meta;
        meta.content_type = std::string{ response[Field::content_type] };
        meta.content_disposition = std::string{ response[Field::content_disposition] };
        meta.etag = std::string{ response[Field::etag] };
        meta.last_modified = std::string{ response[Field::last_modified] };
        auto const cache_control{ response[Field::cache_control] };
        meta.cache_control = std::string{ cache_control };
        meta.must_revalidate = header_has_token(cache_control, "no-cache");
        if (auto const max_age{ header_directive_int(cache_control, "max-age") }; max_age && *max_age >= 0) {
            auto const age{ std::max<std::int64_t>(0, header_directive_int(std::format("age={}", response[Field::age]), "age").value_or(0)) };
            auto const now{ std::chrono::system_clock::now() };
            auto const available{ std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::time_point::max() - now).count() };
            meta.fresh_until = now + std::chrono::seconds{ std::min(available, std::max<std::int64_t>(0, *max_age - age)) };
        }
        return meta;
    }

    auto serialize_meta(HttpMeta const& meta) -> std::string {
        auto object = Json::object();
        for (auto const& [key, value] : {
                 std::pair{                "etag",                &meta.etag },
                 std::pair{       "last_modified",       &meta.last_modified },
                 std::pair{        "content_type",        &meta.content_type },
                 std::pair{ "content_disposition", &meta.content_disposition }
        }) {
            if (!value->empty()) {
                object[key] = *value;
            }
        }
        if (meta.fresh_until) {
            object["fresh_until"] = std::chrono::duration_cast<std::chrono::seconds>(meta.fresh_until->time_since_epoch()).count();
        }
        if (meta.must_revalidate) {
            object["must_revalidate"] = true;
        }
        object["cache_control"] = meta.cache_control;
        return object.dump();
    }

    /**
     * @brief Parse a metadata blob without throwing on unexpected content.
     * @param blob Stored metadata.
     * @return Metadata, or std::nullopt when not an object.
     */
    auto parse_meta(std::string_view blob) -> std::optional<HttpMeta> {
        auto value = Json::parse(blob.begin(), blob.end(), nullptr, false);
        if (value.is_discarded() || !value.is_object()) {
            return std::nullopt;
        }
        auto const text{ [&](char const* key) {
            auto const it{ value.find(key) };
            return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
        } };
        HttpMeta meta;
        meta.etag = text("etag");
        meta.last_modified = text("last_modified");
        meta.content_type = text("content_type");
        meta.content_disposition = text("content_disposition");
        meta.cache_control = text("cache_control");
        if (auto const it{ value.find("fresh_until") }; it != value.end() && it->is_number_integer()) {
            meta.fresh_until = std::chrono::system_clock::time_point{ std::chrono::seconds{ it->get<std::int64_t>() } };
        }
        if (auto const it{ value.find("must_revalidate") }; it != value.end() && it->is_boolean()) {
            meta.must_revalidate = it->get<bool>();
        }
        return meta;
    }

    /**
     * @brief Absolute request location of one hop.
     */
    struct Location {
        std::string host;                         ///< Host.
        std::uint16_t port{ 80 };                 ///< Port.
        url::Scheme scheme{ url::Scheme::Plain }; ///< Transport.
        std::string target{ "/" };                ///< Origin-form target.

        /**
         * @brief Return the canonical URL, the base of the cache key.
         * @return URL text.
         */
        [[nodiscard]] auto Url() const -> std::string {
            return url::make_url_value(host, port, scheme, target);
        }

        [[nodiscard]] auto SameOrigin(Location const& other) const -> bool {
            return iequals(host, other.host) && port == other.port && scheme == other.scheme;
        }
    };

    /**
     * @brief A cache hit with its parsed metadata.
     */
    struct CachedEntry {
        std::unique_ptr<client::Cache::Entry> entry; ///< Live body handle.
        HttpMeta meta;                               ///< Parsed metadata.
    };

    /**
     * @brief Apply the downloader's HTTP policy (keys, metadata) on top of a generic Cache.
     */
    class CacheManager {
    private:
        std::atomic<std::shared_ptr<client::Cache>> m_cache; ///< Backend; null disables caching.

    public:
        auto SetCache(std::shared_ptr<client::Cache> cache) -> void {
            m_cache.store(std::move(cache));
        }

        [[nodiscard]] auto GetCache() const -> std::shared_ptr<client::Cache> {
            return m_cache.load();
        }

        [[nodiscard]] auto Get(Location const& location, Headers const& request) const -> std::optional<CachedEntry> {
            auto const cache{ m_cache.load() };
            if (!cache || header_has_token(request[Field::cache_control], "no-store")) {
                return std::nullopt;
            }
            auto entry{ cache->Get(key(location, request)) };
            if (!entry) {
                return std::nullopt;
            }
            auto meta{ parse_meta(entry->Metadata()) };
            if (!meta) {
                return std::nullopt;
            }
            return CachedEntry{ std::move(entry), std::move(*meta) };
        }

        auto Put(
            Location const& location,
            Headers const& request,
            Headers const& response,
            std::filesystem::path const& body
        ) const -> void {
            if (auto const cache{ m_cache.load() }) {
                cache->Put(key(location, request), body, serialize_meta(make_meta(response)), std::nullopt);
            }
        }

        auto Remove(Location const& location, Headers const& request) const -> void {
            if (auto const cache{ m_cache.load() }) {
                cache->Remove(key(location, request));
            }
        }

        /**
         * @brief Refresh cached validators and policy using a successful 304 response.
         */
        auto Revalidate(
            Location const& location,
            Headers const& request,
            CachedEntry const& cached,
            Headers const& response
        ) const -> HttpMeta {
            Headers merged;
            merged.Set(Field::etag, cached.meta.etag);
            merged.Set(Field::last_modified, cached.meta.last_modified);
            merged.Set(Field::content_type, cached.meta.content_type);
            merged.Set(Field::content_disposition, cached.meta.content_disposition);
            merged.Set(Field::cache_control, cached.meta.cache_control);
            for (auto const& header : response) {
                merged.Set(header.name_string, header.value);
            }
            auto meta{ make_meta(merged) };
            if (auto const cache{ m_cache.load() }) {
                if (header_has_token(merged[Field::cache_control], "no-store") || !merged[Field::vary].empty()) {
                    cache->Remove(key(location, request));
                } else {
                    cache->UpdateMetadata(key(location, request), serialize_meta(meta), std::nullopt);
                }
            }
            return meta;
        }

    private:
        static auto key(Location const& location, Headers const& request) -> std::string {
            auto out{ location.Url() };
            if (auto const scope{ cache_auth_scope(request) }; !scope.empty()) {
                out += "|auth=" + scope;
            }
            return out;
        }
    };

    /**
     * @brief The `<save_path>.part` file of a run.
     */
    class PartFile {
    private:
        std::filesystem::path m_save_path; ///< Final destination.
        std::fstream m_out;                ///< Open part file.

    public:
        static auto PathOf(std::filesystem::path const& save_path) -> std::filesystem::path {
            auto out{ save_path };
            out += ".part";
            return out;
        }

        static auto PartialSize(std::filesystem::path const& save_path) -> std::uint64_t {
            std::error_code ec;
            auto const size{ std::filesystem::file_size(PathOf(save_path), ec) };
            return ec ? 0 : static_cast<std::uint64_t>(size);
        }

        static auto Discard(std::filesystem::path const& save_path) -> void {
            std::error_code ec;
            std::filesystem::remove(PathOf(save_path), ec);
        }

        /**
         * @brief Move the part file onto the destination, replacing it.
         * @param save_path Destination.
         * @return io_error when the rename failed.
         */
        static auto Publish(std::filesystem::path const& save_path) -> boost::system::error_code {
            std::error_code ec;
#if defined(_WIN32)
            if (!::MoveFileExW(PathOf(save_path).c_str(), save_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                return boost::system::error_code{ static_cast<int>(::GetLastError()), boost::system::system_category() };
            }
#else
            std::filesystem::rename(PathOf(save_path), save_path, ec);
            if (ec) {
                return boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
#endif
            return {};
        }

        /**
         * @brief Copy a cached body to the destination through the part file.
         * @param entry Cached entry.
         * @param save_path Destination; appears only once complete.
         * @return Error code.
         */
        static auto CopyAtomic(client::Cache::Entry const& entry, std::filesystem::path const& save_path) -> boost::system::error_code {
            if (auto const ec{ entry.CopyToFile(PathOf(save_path)) }) {
                Discard(save_path);
                return ec;
            }
            return Publish(save_path);
        }

        /**
         * @brief Open the part file for appending at `offset`, or recreate it when 0.
         * @param save_path Destination.
         * @param offset Bytes to keep.
         * @return Error code.
         * @note Resuming opens with in|out: httplib used out alone, which truncates the file,
         *       and then wrote past the end, leaving zeros instead of the kept bytes.
         */
        auto Open(std::filesystem::path const& save_path, std::uint64_t offset) -> boost::system::error_code {
            m_save_path = save_path;
            m_out.close();
            m_out.clear();
            auto const mode{ offset == 0 ? std::ios::out | std::ios::binary | std::ios::trunc : std::ios::in | std::ios::out | std::ios::binary };
            m_out.open(PathOf(save_path), mode);
            if (!m_out.is_open()) {
                return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            }
            if (offset > 0) {
                m_out.seekp(static_cast<std::streamoff>(offset));
                if (!m_out) {
                    m_out.close();
                    return boost::system::errc::make_error_code(boost::system::errc::io_error);
                }
            }
            return {};
        }

        auto Write(std::string_view data) -> boost::system::error_code {
            m_out.write(data.data(), static_cast<std::streamsize>(data.size()));
            return m_out ? boost::system::error_code{} : boost::system::errc::make_error_code(boost::system::errc::no_space_on_device);
        }

        /**
         * @brief Close, keeping the part file for a resume.
         */
        auto Close() -> void {
            m_out.close();
        }

        /**
         * @brief Close and publish the part file as the destination.
         * @return Error code.
         */
        auto Commit() -> boost::system::error_code {
            m_out.close();
            if (m_out.fail()) {
                return boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
            return Publish(m_save_path);
        }
    };

    /**
     * @brief Progress, state, and pause bookkeeping of a downloader.
     */
    class ProgressTracker {
    private:
        using Downloader = client::Downloader;

        util::AsyncEvent m_pause_event;                                                 ///< Wakes a paused run.
        std::atomic<std::shared_ptr<Downloader::ProgressCallback const>> m_progress;    ///< Progress callback.
        std::atomic<std::shared_ptr<Downloader::StateCallback const>> m_state_callback; ///< State callback.
        std::atomic<Downloader::State> m_state{ Downloader::State::Idle };              ///< Current state.
        std::atomic<bool> m_paused{ false };                                            ///< Pause requested.
        mutable std::mutex m_mutex;                                                     ///< Guards the counters below.
        std::uint64_t m_total{ 0 };                                                     ///< File size; 0 = unknown.
        std::uint64_t m_downloaded{ 0 };                                                ///< Bytes on disk.
        std::uint64_t m_attempt_bytes{ 0 };                                             ///< Bytes of this attempt.
        std::chrono::steady_clock::time_point m_started;                                ///< Attempt start.

    public:
        explicit ProgressTracker(net::any_io_executor executor) : m_pause_event{ std::move(executor) } {}

        auto SetProgressCallback(Downloader::ProgressCallback callback) -> void {
            m_progress.store(std::make_shared<Downloader::ProgressCallback const>(std::move(callback)));
        }

        auto SetStateCallback(Downloader::StateCallback callback) -> void {
            m_state_callback.store(std::make_shared<Downloader::StateCallback const>(std::move(callback)));
        }

        auto Start(std::uint64_t total, std::uint64_t already) -> void {
            std::lock_guard lock{ m_mutex };
            m_total = total;
            m_downloaded = already;
            m_attempt_bytes = 0;
            m_started = std::chrono::steady_clock::now();
        }

        auto Update(std::uint64_t bytes) -> void {
            Downloader::ProgressInfo info;
            {
                std::lock_guard lock{ m_mutex };
                m_downloaded += bytes;
                m_attempt_bytes += bytes;
                auto const elapsed{ std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count() };
                info.total_bytes = m_total;
                info.downloaded_bytes = m_downloaded;
                if (elapsed > 0.0) {
                    info.speed_bytes_per_sec = static_cast<std::uint64_t>(static_cast<double>(m_attempt_bytes) / elapsed);
                }
                if (m_total > m_downloaded && info.speed_bytes_per_sec > 0) {
                    info.eta = std::chrono::seconds{ static_cast<std::int64_t>((m_total - m_downloaded) / info.speed_bytes_per_sec) };
                }
            }
            report(info);
        }

        /**
         * @brief Report the final size of a completed run.
         * @param size Bytes, or 0 to use the tracked size.
         */
        auto Finish(std::uint64_t size = 0) -> void {
            if (size == 0) {
                std::lock_guard lock{ m_mutex };
                size = m_total > 0 ? m_total : m_downloaded;
            }
            report(Downloader::ProgressInfo{ size, size, 0, std::chrono::seconds{ 0 } });
        }

        /**
         * @brief Wait while paused.
         * @param cancelled Cancel flag checked after every wakeup.
         * @return operation_aborted when cancelled during the pause.
         */
        auto WaitIfPaused(std::atomic<bool> const& cancelled) -> net::awaitable<boost::system::error_code> {
            if (!m_paused.load()) {
                co_return boost::system::error_code{};
            }
            while (m_paused.load()) {
                SetState(Downloader::State::Paused, {});
                co_await m_pause_event.Wait();
                if (cancelled.load()) {
                    co_return make_error_code(net::error::operation_aborted);
                }
            }
            SetState(Downloader::State::Downloading, {});
            co_return boost::system::error_code{};
        }

        /**
         * @brief Change state, calling the state callback when it changed.
         */
        auto SetState(Downloader::State state, boost::system::error_code ec) -> void {
            if (m_state.exchange(state) == state) {
                return;
            }
            if (auto const callback{ m_state_callback.load() }; callback && *callback) {
                (*callback)(state, ec);
            }
        }

        auto ResetState() -> void {
            m_state.store(Downloader::State::Idle);
        }

        [[nodiscard]] auto GetState() const -> Downloader::State {
            return m_state.load();
        }

        auto Pause() -> void {
            m_paused.store(true);
        }

        auto Resume() -> void {
            m_paused.store(false);
            m_pause_event.NotifyAll();
        }

        [[nodiscard]] auto IsPaused() const -> bool {
            return m_paused.load();
        }

        auto Wake() -> void {
            m_pause_event.NotifyAll();
        }

    private:
        auto report(Downloader::ProgressInfo const& info) const -> void {
            if (auto const callback{ m_progress.load() }; callback && *callback) {
                (*callback)(info);
            }
        }
    };

    /**
     * @brief Report whether a non-success status may succeed when repeated.
     * @param status Response status.
     * @return True for 408, 429, and 5xx.
     */
    auto is_transient_status(Status status) -> bool {
        return status == Status::request_timeout || status == Status::too_many_requests || static_cast<unsigned>(status) >= 500;
    }

} // namespace mhttp::detail

namespace mhttp::client {

    /**
     * @brief State behind Downloader.
     */
    class DownloaderImpl : public std::enable_shared_from_this<DownloaderImpl> {
    private:
        using State = Downloader::State;

        static constexpr std::size_t READ_BUFFER_SIZE{ 64 * 1024 };

        /**
         * @brief Terminal response of one logical request after redirects.
         */
        struct Sent {
            HttpClientPool::ClientHandle handle;       ///< Connection owning the lazy body.
            Response response;                         ///< Response; body unread.
            detail::Location location;                 ///< URL that served the response.
            std::unique_ptr<Cache::Entry> cache_entry; ///< Entry whose validators were sent.
            detail::HttpMeta cache_meta;               ///< Policy of that entry.
            Headers request_headers;                   ///< Credentials of the final hop.
        };

        /**
         * @brief Result of a single-stream download.
         */
        struct Payload {
            Headers headers;           ///< Response fields, for the cache.
            detail::Location location; ///< URL that served the body.
            bool from_cache{ false };  ///< A 304 answered from the cache.
            Headers request_headers;   ///< Final-hop cache scope.
        };

        /**
         * @brief Register the client in use, so Cancel() can close it from any thread.
         */
        class ActiveClient {
        private:
            DownloaderImpl* m_owner{ nullptr }; ///< Owner; null once moved from.

        public:
            ActiveClient(DownloaderImpl& owner, HttpClient& client) : m_owner{ &owner } {
                owner.setActive(&client);
            }

            ActiveClient(ActiveClient const&) = delete;
            auto operator=(ActiveClient const&) -> ActiveClient& = delete;

            ~ActiveClient() {
                if (m_owner) {
                    m_owner->setActive(nullptr);
                }
            }
        };

        net::any_io_executor m_executor;                      ///< Timers and background runs.
        std::shared_ptr<HttpClientPool> m_pool;               ///< Connection source.
        detail::CacheManager m_cache;                         ///< Optional response cache.
        detail::ProgressTracker m_progress;                   ///< Progress and pause state.
        util::AsyncEvent m_wake;                              ///< Interrupts retry backoffs on Cancel().
        std::atomic<bool> m_cancelled{ false };               ///< Cancel requested for this run.
        std::atomic<bool> m_running{ false };                 ///< A run is active.
        std::atomic<Status> m_http_status{ Status::unknown }; ///< Last response status.
        mutable std::mutex m_mutex;                           ///< Guards the members below.
        Downloader::Config m_config;                          ///< Settings.
        std::string m_suggested_filename;                     ///< From Content-Disposition.
        HttpClient* m_active{ nullptr };                      ///< Client currently used by the run.

    public:
        DownloaderImpl(
            net::any_io_executor executor,
            std::shared_ptr<HttpClientPool> pool
        ) : m_executor{ executor }, m_pool{ std::move(pool) }, m_progress{ executor }, m_wake{ executor } {}

        auto SetConfig(Downloader::Config const& config) -> void {
            std::lock_guard lock{ m_mutex };
            m_config = config;
        }

        [[nodiscard]] auto GetConfig() const -> Downloader::Config {
            std::lock_guard lock{ m_mutex };
            return m_config;
        }

        [[nodiscard]] auto Progress() -> detail::ProgressTracker& {
            return m_progress;
        }

        [[nodiscard]] auto CacheManager() -> detail::CacheManager& {
            return m_cache;
        }

        [[nodiscard]] auto Pool() const -> std::shared_ptr<HttpClientPool> {
            return m_pool;
        }

        [[nodiscard]] auto Executor() const -> net::any_io_executor {
            return m_executor;
        }

        [[nodiscard]] auto SuggestedFilename() const -> std::string {
            std::lock_guard lock{ m_mutex };
            return m_suggested_filename;
        }

        [[nodiscard]] auto LastStatus() const -> Status {
            return m_http_status.load();
        }

        auto Cancel() -> void {
            m_cancelled.store(true);
            {
                std::lock_guard lock{ m_mutex };
                if (m_active) {
                    static_cast<void>(m_active->Close());
                }
            }
            m_progress.Wake();
            m_wake.NotifyAll();
        }

        auto AsyncDownload(
            std::string_view url_text,
            std::filesystem::path const& save_path,
            Headers const& headers
        ) -> net::awaitable<boost::system::error_code> {
            if (m_running.exchange(true)) {
                co_return make_error_code(Error::AlreadyInUse);
            }

            /**
             * @brief Release the run slot even when a callback throws or the coroutine is cancelled.
             */
            struct RunGuard {
                std::atomic<bool>& running;

                ~RunGuard() { running.store(false); }
            } const guard{ m_running };

            m_cancelled.store(false);
            m_http_status.store(Status::unknown);
            m_progress.ResetState();
            m_progress.Start(0, 0);
            {
                std::lock_guard lock{ m_mutex };
                m_suggested_filename.clear();
            }
            while (m_wake.TryWait()) {
            }
            Headers owned;
            owned.Merge(headers);
            auto const ec{ co_await run(std::string{ url_text }, save_path, owned) };
            co_return ec;
        }

    private:
        auto setActive(HttpClient* client) -> void {
            std::lock_guard lock{ m_mutex };
            m_active = client;
            // A Cancel() that raced with acquiring the client still reaches it.
            if (client && m_cancelled.load()) {
                static_cast<void>(client->Close());
            }
        }

        [[nodiscard]] auto cancelled() const -> bool {
            return m_cancelled.load();
        }

        static auto aborted() -> boost::system::error_code {
            return make_error_code(net::error::operation_aborted);
        }

        /**
         * @brief Wait before a retry, cut short by Cancel().
         * @param config Settings.
         * @param attempt Attempt that failed, from 0.
         */
        auto backoff(Downloader::Config const& config, int attempt) -> net::awaitable<void> {
            if (config.retry_backoff.count() > 0 && !cancelled()) {
                static_cast<void>(co_await m_wake.WaitFor(config.retry_backoff * (attempt + 1)));
            }
        }

        auto finishRun(boost::system::error_code ec) -> boost::system::error_code {
            if (!ec) {
                m_progress.SetState(State::Completed, {});
            } else if (ec == net::error::operation_aborted) {
                m_progress.SetState(State::Cancelled, ec);
            } else {
                m_progress.SetState(State::Failed, ec);
            }
            return ec;
        }

        auto run(
            std::string url_text,
            std::filesystem::path save_path,
            Headers headers
        ) -> net::awaitable<boost::system::error_code> {
            auto const config{ GetConfig() };
            auto const parsed{ url::parse_url(url_text) };
            if (!parsed || !(detail::iequals(parsed->scheme, "http") || detail::iequals(parsed->scheme, "https"))) {
                co_return finishRun(make_error_code(Error::InvalidUrl));
            }
            detail::Location const origin{ parsed->host, parsed->EffectivePort(), parsed->Transport(), parsed->Target() };
            m_progress.SetState(State::Connecting, {});
            if (cancelled()) {
                co_return finishRun(aborted());
            }
            // A fresh entry for the requested URL needs no round trip at all.
            if (auto cached{ m_cache.Get(origin, headers) }; cached && !detail::header_has_token(headers[Field::cache_control], "no-cache") && serveFresh(*cached, save_path)) {
                co_return finishRun({});
            }
            m_progress.SetState(State::Downloading, {});
            auto const payload{ co_await downloadSingle(origin, save_path, config, headers) };
            if (!payload) {
                co_return finishRun(cancelled() ? aborted() : payload.error());
            }
            if (!payload->from_cache && !detail::header_has_token(headers[Field::cache_control], "no-store") && !detail::header_has_token(payload->headers[Field::cache_control], "no-store") && payload->headers[Field::vary].empty()) {
                m_cache.Put(payload->location, payload->request_headers, payload->headers, save_path);
            } else if (!payload->from_cache) {
                m_cache.Remove(payload->location, payload->request_headers);
            }
            co_return finishRun({});
        }

        auto serveFresh(detail::CachedEntry const& cached, std::filesystem::path const& save_path) -> bool {
            if (cached.meta.must_revalidate || !cached.meta.fresh_until || std::chrono::system_clock::now() >= *cached.meta.fresh_until) {
                return false;
            }
            m_progress.SetState(State::Downloading, {});
            if (detail::PartFile::CopyAtomic(*cached.entry, save_path)) {
                return false;
            }
            m_progress.Finish(cached.entry->Size());
            setSuggestedFilename(cached.meta.content_disposition);
            return true;
        }

        /**
         * @brief Send one GET and follow redirects, attaching each hop's cached validators.
         * @param origin Requested location.
         * @param request_headers Merged request fields.
         * @param config Settings.
         * @return Terminal response with its connection, or the error.
         */
        auto send(
            detail::Location const& origin,
            Headers request_headers,
            Downloader::Config const& config
        ) -> net::awaitable<Result<Sent>> {
            // Resuming relies on raw bytes, so no content coding.
            request_headers.Set(Field::accept_encoding, "identity");
            auto hop{ origin };
            for (int redirect{ 0 };; ++redirect) {
                if (cancelled()) {
                    co_return std::unexpected{ aborted() };
                }
                auto handle{ co_await m_pool->AsyncAcquire(hop.host, hop.port, hop.scheme, config.acquire_timeout) };
                if (!handle) {
                    co_return std::unexpected{ handle.GetError() };
                }
                auto& client{ *handle };
                // Per-step timeouts: an overall limit would cut off large or slow bodies.
                client.SetTimeoutPolicy(TimeoutPolicy::Step);
                client.SetTimeout(config.timeout);
                client.SetMaxRedirects(0);
                client.SetDownloadRateLimit(config.max_speed_bytes_per_sec);

                request_headers.Erase(Field::if_none_match);
                request_headers.Erase(Field::if_modified_since);
                std::unique_ptr<Cache::Entry> hop_entry;
                detail::HttpMeta hop_meta;
                if (auto cached{ m_cache.Get(hop, request_headers) }) {
                    if (!cached->meta.etag.empty()) {
                        request_headers.Set(Field::if_none_match, cached->meta.etag);
                    }
                    if (!cached->meta.last_modified.empty()) {
                        request_headers.Set(Field::if_modified_since, cached->meta.last_modified);
                    }
                    hop_entry = std::move(cached->entry);
                    hop_meta = std::move(cached->meta);
                }

                Result<Response> result;
                {
                    ActiveClient const active{ *this, client };
                    Request request{ Verb::get, hop.target, request_headers };
                    result = co_await client.AsyncSendRequest(request, BodyMode::Lazy);
                }
                if (!result) {
                    co_return std::unexpected{ cancelled() ? aborted() : result.error() };
                }
                auto const status{ result->Result() };
                std::string const location{ (*result)[Field::location] };
                bool const is_redirect{ status == Status::moved_permanently || status == Status::found || status == Status::see_other || status == Status::temporary_redirect || status == Status::permanent_redirect };
                if (!is_redirect || location.empty()) {
                    co_return Sent{ std::move(handle), std::move(*result), std::move(hop), std::move(hop_entry), std::move(hop_meta), std::move(request_headers) };
                }
                {
                    ActiveClient const active{ *this, client };
                    if (co_await result->Drain()) {
                        static_cast<void>(client.Close());
                    }
                }
                if (redirect >= config.max_redirects) {
                    co_return std::unexpected{ make_error_code(Error::TooManyRedirects) };
                }
                auto next{ nextHop(hop, location) };
                if (!next) {
                    co_return std::unexpected{ next.error() };
                }
                if (!next->SameOrigin(hop)) {
                    for (auto const field : { Field::authorization, Field::proxy_authorization, Field::cookie }) {
                        request_headers.Erase(field);
                    }
                    request_headers.Erase("Cookie2");
                    request_headers.Erase(Field::host);
                }
                hop = std::move(*next);
            }
        }

        /**
         * @brief Resolve a Location against the current hop.
         * @return Next hop, or Error::InvalidUrl for a non-HTTP or malformed URL.
         */
        static auto nextHop(detail::Location const& current, std::string location) -> Result<detail::Location> {
            if (location.starts_with("//")) {
                location = std::format("{}:{}", url::to_string(current.scheme), location);
            }
            auto const separator{ location.find("://") };
            bool const absolute{ separator != std::string::npos && separator > 0 && location.find_first_of("/?#") > separator };
            if (!absolute) {
                auto next{ current };
                next.target = url::resolve(current.target, location);
                return next;
            }
            auto const parsed{ url::parse_url(location) };
            if (!parsed || !(detail::iequals(parsed->scheme, "http") || detail::iequals(parsed->scheme, "https"))) {
                return std::unexpected{ make_error_code(Error::InvalidUrl) };
            }
            return detail::Location{ parsed->host, parsed->EffectivePort(), parsed->Transport(), parsed->Target() };
        }

        /**
         * @brief Download the body with resume and retries.
         * @return Payload, or the error.
         */
        auto downloadSingle(
            detail::Location const& origin,
            std::filesystem::path const& save_path,
            Downloader::Config const& config,
            Headers const& base_headers
        ) -> net::awaitable<Result<Payload>> {
            std::vector<char> buffer(READ_BUFFER_SIZE);
            for (int attempt{ 0 };; ++attempt) {
                if (cancelled()) {
                    co_return std::unexpected{ aborted() };
                }
                if (auto const ec{ co_await m_progress.WaitIfPaused(m_cancelled) }) {
                    co_return std::unexpected{ ec };
                }
                // httplib only looked at the part file on the first attempt, so retries
                // started over instead of resuming.
                if (!config.resume) {
                    detail::PartFile::Discard(save_path);
                }
                auto existing{ config.resume ? detail::PartFile::PartialSize(save_path) : 0 };
                Headers request_headers;
                request_headers.Merge(base_headers);
                if (existing > 0) {
                    request_headers.Set(Field::range, std::format("bytes={}-", existing));
                }

                auto result{ co_await send(origin, request_headers, config) };
                if (!result) {
                    auto const ec{ result.error() };
                    if (cancelled() || ec == Error::TooManyRedirects || ec == Error::InvalidUrl || attempt >= config.max_retries) {
                        co_return std::unexpected{ ec };
                    }
                    co_await backoff(config, attempt);
                    continue;
                }
                std::optional<Sent> sent{ std::move(*result) };
                auto status{ sent->response.Result() };
                m_http_status.store(status);

                if (status == Status::not_modified) {
                    if (sent->cache_entry && !detail::PartFile::CopyAtomic(*sent->cache_entry, save_path)) {
                        m_progress.Finish(sent->cache_entry->Size());
                        detail::CachedEntry const cached{ std::move(sent->cache_entry), sent->cache_meta };
                        auto const meta{ m_cache.Revalidate(sent->location, sent->request_headers, cached, sent->response.Base()) };
                        setSuggestedFilename(meta.content_disposition);
                        co_return Payload{ Headers{}, sent->location, true, sent->request_headers };
                    }
                    // The cached body is gone: drop the entry and ask again unconditionally.
                    auto const location{ sent->location };
                    auto const cache_headers{ sent->request_headers };
                    sent.reset();
                    m_cache.Remove(location, cache_headers);
                    auto retry{ co_await send(origin, request_headers, config) };
                    if (!retry) {
                        co_return std::unexpected{ retry.error() };
                    }
                    sent.emplace(std::move(*retry));
                    status = sent->response.Result();
                    m_http_status.store(status);
                    if (status == Status::not_modified) {
                        co_return std::unexpected{ make_error_code(Error::CacheUnavailable) };
                    }
                }

                if (status != Status::ok && status != Status::partial_content) {
                    {
                        ActiveClient const active{ *this, *sent->handle };
                        if (co_await sent->response.Drain()) {
                            static_cast<void>(sent->handle->Close());
                        }
                    }
                    if (status == Status::range_not_satisfiable && existing > 0 && attempt < config.max_retries) {
                        detail::PartFile::Discard(save_path);
                        sent.reset();
                        continue;
                    }
                    if (status == Status::not_found || status == Status::gone) {
                        // A gone resource must not keep sending dead validators.
                        sent->cache_entry.reset();
                        m_cache.Remove(sent->location, sent->request_headers);
                    }
                    if (!detail::is_transient_status(status) || attempt >= config.max_retries) {
                        co_return std::unexpected{ make_error_code(Error::HttpStatus) };
                    }
                    sent.reset();
                    co_await backoff(config, attempt);
                    continue;
                }

                auto& response{ sent->response };
                auto const range{ detail::parse_content_range(response[Field::content_range]) };
                if (status == Status::ok) {
                    existing = 0; // the server ignored Range
                } else if (!range || range->start != existing) {
                    // A 206 at another offset would corrupt the appended data.
                    if (attempt >= config.max_retries) {
                        co_return std::unexpected{ make_error_code(Error::ProtocolViolation) };
                    }
                    detail::PartFile::Discard(save_path);
                    sent.reset();
                    co_await backoff(config, attempt);
                    continue;
                }
                auto const content_length{ response.ContentLength() };
                bool const encoded{ !response[Field::content_encoding].empty() && !detail::iequals(response[Field::content_encoding], "identity") };
                if (encoded || (status == Status::partial_content && content_length && *content_length != range->end - range->start + 1)) {
                    co_return std::unexpected{ make_error_code(Error::ProtocolViolation) };
                }
                std::uint64_t const total{ status == Status::partial_content ? (range->total > 0 ? range->total : range->end + 1) : content_length.value_or(0) };
                m_progress.Start(total, existing);

                detail::PartFile file;
                if (auto const ec{ file.Open(save_path, existing) }) {
                    co_return std::unexpected{ ec };
                }
                std::uint64_t received{ 0 };
                std::optional<boost::system::error_code> interrupted;
                {
                    ActiveClient const active{ *this, *sent->handle };
                    while (!response.IsBodyDone()) {
                        if (cancelled()) {
                            file.Close();
                            co_return std::unexpected{ aborted() };
                        }
                        if (auto const ec{ co_await m_progress.WaitIfPaused(m_cancelled) }) {
                            file.Close();
                            co_return std::unexpected{ ec };
                        }
                        boost::system::error_code ec;
                        auto const bytes{ co_await response.ReadSomeRaw(net::buffer(buffer), ec) };
                        if (auto const write_ec{ file.Write(std::string_view{ buffer.data(), bytes }) }) {
                            file.Close();
                            co_return std::unexpected{ write_ec };
                        }
                        received += bytes;
                        m_progress.Update(bytes);
                        if (ec) {
                            interrupted = cancelled() ? aborted() : ec;
                            break;
                        }
                        if (bytes == 0) {
                            break;
                        }
                    }
                }
                if (interrupted) {
                    // httplib failed at once here; keep the bytes and resume on the next attempt.
                    file.Close();
                    if (cancelled() || attempt >= config.max_retries) {
                        co_return std::unexpected{ *interrupted };
                    }
                    sent.reset();
                    co_await backoff(config, attempt);
                    continue;
                }
                if (cancelled()) {
                    file.Close();
                    co_return std::unexpected{ aborted() };
                }
                if (total > existing && received != total - existing) {
                    file.Close();
                    if (attempt >= config.max_retries) {
                        co_return std::unexpected{ make_error_code(Error::TruncatedBody) };
                    }
                    sent.reset();
                    co_await backoff(config, attempt);
                    continue;
                }
                if (auto const ec{ file.Commit() }) {
                    co_return std::unexpected{ ec };
                }
                setSuggestedFilename(response[Field::content_disposition]);
                m_progress.Finish();
                Payload payload;
                payload.headers.Merge(response.Base());
                payload.location = sent->location;
                payload.request_headers = sent->request_headers;
                co_return payload;
            }
        }

        /**
         * @brief Remember the current representation's safe suggested filename.
         */
        auto setSuggestedFilename(std::string_view disposition) -> void {
            std::lock_guard lock{ m_mutex };
            m_suggested_filename = detail::parse_content_disposition_filename(disposition);
        }
    };

    Downloader::Downloader(
        net::any_io_executor const& executor,
        std::shared_ptr<HttpClientPool> pool
    ) : m_impl{ std::make_shared<DownloaderImpl>(executor, std::move(pool)) } {}

    Downloader::Downloader(
        net::io_context& context,
        std::shared_ptr<HttpClientPool> pool
    ) : Downloader{ context.get_executor(), std::move(pool) } {}

    Downloader::~Downloader() {
        if (m_impl) {
            m_impl->Cancel();
        }
    }

    auto Downloader::operator=(Downloader&& other) noexcept -> Downloader& {
        if (this != &other) {
            if (m_impl) {
                m_impl->Cancel();
            }
            m_impl = std::move(other.m_impl);
        }
        return *this;
    }

    auto Downloader::SetConfig(Config const& config) -> void {
        m_impl->SetConfig(config);
    }

    auto Downloader::GetConfig() const -> Config {
        return m_impl->GetConfig();
    }

    auto Downloader::SetProgressCallback(ProgressCallback callback) -> void {
        m_impl->Progress().SetProgressCallback(std::move(callback));
    }

    auto Downloader::SetStateCallback(StateCallback callback) -> void {
        m_impl->Progress().SetStateCallback(std::move(callback));
    }

    auto Downloader::SetCache(std::shared_ptr<Cache> cache) -> void {
        m_impl->CacheManager().SetCache(std::move(cache));
    }

    auto Downloader::GetCache() const -> std::shared_ptr<Cache> {
        return m_impl->CacheManager().GetCache();
    }

    auto Downloader::GetPool() const -> std::shared_ptr<HttpClientPool> {
        return m_impl->Pool();
    }

    auto Downloader::AsyncDownload(
        std::string_view url,
        std::filesystem::path const& save_path,
        Headers const& headers
    ) -> net::awaitable<boost::system::error_code> {
        auto const impl{ m_impl };
        co_return co_await impl->AsyncDownload(url, save_path, headers);
    }

    auto Downloader::Download(
        std::string_view url,
        std::filesystem::path const& save_path,
        Headers const& headers
    ) -> std::future<boost::system::error_code> {
        Headers owned;
        owned.Merge(headers);
        auto const impl{ m_impl };
        return net::co_spawn(
            impl->Executor(),
            [impl, url = std::string{ url }, save_path, owned = std::move(owned)]() -> net::awaitable<boost::system::error_code> {
                co_return co_await impl->AsyncDownload(url, save_path, owned);
            },
            net::use_future
        );
    }

    auto Downloader::Cancel() -> void {
        m_impl->Cancel();
    }

    auto Downloader::Pause() -> void {
        m_impl->Progress().Pause();
    }

    auto Downloader::Resume() -> void {
        m_impl->Progress().Resume();
    }

    auto Downloader::IsPaused() const -> bool {
        return m_impl->Progress().IsPaused();
    }

    auto Downloader::SuggestedFilename() const -> std::string {
        return m_impl->SuggestedFilename();
    }

    auto Downloader::CurrentState() const -> State {
        return m_impl->Progress().GetState();
    }

    auto Downloader::HttpStatus() const -> Status {
        return m_impl->LastStatus();
    }

} // namespace mhttp::client
