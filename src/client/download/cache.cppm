/**
 * @file cache.cppm
 * @brief Provide the downloader's keyed blob store: the Cache interface and DiskCache.
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
#else
    #include <fcntl.h>
    #include <sys/file.h>
    #include <unistd.h>
#endif

#include <boost/system/errc.hpp>
#include <boost/system/error_code.hpp>

export module mhttp:cache;

import std;
import :config;

export namespace mhttp::client {

    /**
     * @brief Keyed blob store used by Downloader (port of httplib::client::cache).
     *
     * The cache knows nothing about HTTP: it maps an opaque caller-chosen key to a body plus an
     * opaque metadata blob, and evicts entries by expiry and size policy.
     */
    class Cache {
    public:
        using Clock = std::chrono::system_clock;
        using TimePoint = Clock::time_point;

        /**
         * @brief One stored entry, a live handle that keeps its body readable.
         *
         * The backend keeps the body readable while the entry is alive (DiskCache holds the
         * body file open), so a concurrent eviction does not invalidate a handed-out entry;
         * reads report failure instead of returning wrong data.
         */
        class Entry {
        public:
            virtual ~Entry() = default;

            Entry(Entry const&) = delete;
            auto operator=(Entry const&) -> Entry& = delete;

            /**
             * @brief Return the caller-defined metadata, stored verbatim.
             * @return Metadata bytes.
             */
            [[nodiscard]] virtual auto Metadata() const -> std::string_view = 0;

            /**
             * @brief Return the per-entry deadline.
             * @return Deadline, or std::nullopt to use the cache's default max age.
             */
            [[nodiscard]] virtual auto ExpiresAt() const -> std::optional<TimePoint> = 0;

            /**
             * @brief Return the body size observed when the entry was fetched.
             * @return Bytes.
             */
            [[nodiscard]] virtual auto Size() const -> std::uint64_t = 0;

            /**
             * @brief Read the whole body.
             * @return Body, or std::nullopt when unreadable.
             */
            [[nodiscard]] virtual auto ReadAll() const -> std::optional<std::string> = 0;

            /**
             * @brief Copy the body to a file without buffering it whole; not atomic.
             * @param destination File to create or truncate.
             * @return Error when the body is unreadable or the file cannot be written.
             */
            virtual auto CopyToFile(std::filesystem::path const& destination) const -> boost::system::error_code = 0;

        protected:
            Entry() = default;
        };

        virtual ~Cache() = default;

        Cache(Cache const&) = delete;
        auto operator=(Cache const&) -> Cache& = delete;

        /**
         * @brief Return the live entry for a key.
         * @param key Caller-defined key.
         * @return Entry, or nullptr when absent or expired.
         */
        virtual auto Get(std::string_view key) -> std::unique_ptr<Entry> = 0;

        /**
         * @brief Store a body and metadata, replacing any previous entry.
         * @param key Caller-defined key.
         * @param source_body File to copy; when missing, an existing entry stays untouched.
         * @param metadata Opaque metadata.
         * @param expires_at Optional per-entry deadline.
         */
        virtual auto Put(
            std::string_view key,
            std::filesystem::path const& source_body,
            std::string_view metadata,
            std::optional<TimePoint> expires_at
        ) -> void = 0;

        /**
         * @brief Replace only the metadata and deadline of a live entry (e.g. after a 304).
         * @return False when there is no live entry.
         */
        virtual auto UpdateMetadata(
            std::string_view key,
            std::string_view metadata,
            std::optional<TimePoint> expires_at
        ) -> bool = 0;

        virtual auto Remove(std::string_view key) -> void = 0;

    protected:
        Cache() = default;
    };

    /**
     * @brief Cache stored in a directory (port of httplib::client::disk_cache).
     *
     * Each entry is a directory `<dir>/<hash[0..2]>/<hash>/` holding `body`, `meta`, `key`,
     * `version`, and an optional `expires` file; it is staged under `<hash>.tmp` and renamed into
     * place. Entries without a deadline expire `max_age` after their last use; when the total
     * exceeds `max_size`, the least recently used entries go first. The `key` file guards
     * against hash collisions, which httplib answered with another key's body. Thread-safe; a
     * lock file keeps a second instance or process from sharing the directory.
     */
    class DiskCache : public Cache {
    private:
        class Impl;

        std::unique_ptr<Impl> m_impl; ///< Cache state.

    public:
        /**
         * @brief Open or create a cache directory.
         * @param directory Directory; created when missing.
         * @throws std::runtime_error When the directory cannot be created or locked.
         */
        explicit DiskCache(std::filesystem::path directory);
        ~DiskCache() override;

        auto Get(std::string_view key) -> std::unique_ptr<Entry> override;

        auto Put(
            std::string_view key,
            std::filesystem::path const& source_body,
            std::string_view metadata,
            std::optional<TimePoint> expires_at
        ) -> void override;

        auto UpdateMetadata(
            std::string_view key,
            std::string_view metadata,
            std::optional<TimePoint> expires_at
        ) -> bool override;

        auto Remove(std::string_view key) -> void override;

        /**
         * @brief Remove every entry; the lock file stays.
         */
        auto Clear() -> void;

        /**
         * @brief Drop expired, foreign-version, and over-budget entries and staging leftovers.
         */
        auto Cleanup() -> void;

        /**
         * @brief Set the size budget.
         * @param max_bytes Total body bytes; 0 means unlimited. Default 512 MiB.
         */
        auto SetMaxSize(std::uint64_t max_bytes) -> void;

        /**
         * @brief Set the default lifetime of entries without a deadline.
         * @param max_age Lifetime since last use; 0 means unlimited. Default 7 days.
         */
        auto SetMaxAge(std::chrono::seconds max_age) -> void;

        [[nodiscard]] auto TotalSize() const -> std::uint64_t;
        [[nodiscard]] auto EntryCount() const -> std::size_t;
        [[nodiscard]] auto Directory() const -> std::filesystem::path const&;
    };

} // namespace mhttp::client

namespace mhttp::detail {

    constexpr std::string_view CACHE_BODY_FILE{ "body" };
    constexpr std::string_view CACHE_META_FILE{ "meta" };
    constexpr std::string_view CACHE_KEY_FILE{ "key" };
    constexpr std::string_view CACHE_EXPIRES_FILE{ "expires" };
    constexpr std::string_view CACHE_VERSION_FILE{ "version" };
    constexpr std::string_view CACHE_LOCK_FILE{ ".lock" };
    constexpr int CACHE_FORMAT_VERSION{ 1 }; ///< Bump when the entry layout changes.

    /**
     * @brief Hash bytes with 64-bit FNV-1a.
     * @param text Input.
     * @return 16 lowercase hex digits.
     */
    auto fnv1a_hex(std::string_view text) -> std::string {
        std::uint64_t hash{ 14695981039346656037ULL };
        for (auto const c : text) {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            hash *= 1099511628211ULL;
        }
        return std::format("{:016x}", hash);
    }

    auto is_hex_name(std::string_view name, std::size_t length) -> bool {
        return name.size() == length && std::ranges::all_of(name, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
    }

    auto read_text_file(std::filesystem::path const& path) -> std::optional<std::string> {
        std::ifstream file{ path, std::ios::binary };
        if (!file.is_open()) {
            return std::nullopt;
        }
        return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
    }

    auto write_text_file(std::filesystem::path const& path, std::string_view data) -> bool {
        std::ofstream file{ path, std::ios::binary | std::ios::trunc };
        if (!file.is_open()) {
            return false;
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.close();
        return !file.fail();
    }

    auto parse_int(std::optional<std::string> const& text) -> std::optional<std::int64_t> {
        if (!text || text->empty()) {
            return std::nullopt;
        }
        std::int64_t value{ 0 };
        auto const [end, ec]{ std::from_chars(text->data(), text->data() + text->size(), value) };
        if (ec != std::errc{} || end != text->data() + text->size()) {
            return std::nullopt;
        }
        return value;
    }

    auto to_unix_seconds(client::Cache::TimePoint time) -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    }

    auto remove_tree(std::filesystem::path const& path) -> void {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    /**
     * @brief Collect entry directories `<dir>/<xx>/<hash>`.
     * @param directory Cache directory.
     * @return Entry directories.
     */
    auto collect_entry_dirs(std::filesystem::path const& directory) -> std::vector<std::filesystem::path> {
        std::vector<std::filesystem::path> out;
        std::error_code ec;
        for (auto const& shard : std::filesystem::directory_iterator{ directory, ec }) {
            std::error_code type_ec;
            if (!shard.is_directory(type_ec) || !is_hex_name(shard.path().filename().string(), 2)) {
                continue;
            }
            std::error_code leaf_ec;
            for (auto const& leaf : std::filesystem::directory_iterator{ shard.path(), leaf_ec }) {
                if (leaf.is_directory(type_ec) && is_hex_name(leaf.path().filename().string(), 16)) {
                    out.push_back(leaf.path());
                }
            }
        }
        return out;
    }

    /**
     * @brief DiskCache entry holding its body file open, so eviction cannot pull it away.
     */
    class DiskEntry final : public client::Cache::Entry {
    private:
        std::uint64_t m_size;                                 ///< Body size at fetch time.
        std::string m_metadata;                               ///< Stored metadata.
        std::optional<client::Cache::TimePoint> m_expires_at; ///< Per-entry deadline.
        mutable std::mutex m_mutex;                           ///< Serializes use of the stream cursor.
#if defined(_WIN32)
        HANDLE m_file{ INVALID_HANDLE_VALUE };                ///< Open body with delete sharing for live handles.
#else
        mutable std::ifstream m_file; ///< Open body.
#endif

    public:
        DiskEntry(
            std::filesystem::path const& body,
            std::uint64_t size,
            std::string metadata,
            std::optional<client::Cache::TimePoint> expires_at
        ) : m_size{ size }, m_metadata{ std::move(metadata) }, m_expires_at{ expires_at } {
#if defined(_WIN32)
            m_file = ::CreateFileW(body.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
#else
            m_file.open(body, std::ios::binary);
#endif
        }

        ~DiskEntry() override {
#if defined(_WIN32)
            if (m_file != INVALID_HANDLE_VALUE) {
                ::CloseHandle(m_file);
            }
#endif
        }

        [[nodiscard]] auto IsOpen() const -> bool {
#if defined(_WIN32)
            return m_file != INVALID_HANDLE_VALUE;
#else
            return m_file.is_open();
#endif
        }

        [[nodiscard]] auto Metadata() const -> std::string_view override {
            return m_metadata;
        }

        [[nodiscard]] auto ExpiresAt() const -> std::optional<client::Cache::TimePoint> override {
            return m_expires_at;
        }

        [[nodiscard]] auto Size() const -> std::uint64_t override {
            return m_size;
        }

        [[nodiscard]] auto ReadAll() const -> std::optional<std::string> override {
            std::lock_guard lock{ m_mutex };
            if (!rewind()) {
                return std::nullopt;
            }
            std::string out(static_cast<std::size_t>(m_size), '\0');
            std::size_t offset{ 0 };
            while (offset < out.size()) {
                auto const got{ readSome(out.data() + offset, out.size() - offset) };
                if (got == 0) {
                    return std::nullopt;
                }
                offset += got;
            }
            return out;
        }

        auto CopyToFile(std::filesystem::path const& destination) const -> boost::system::error_code override {
            std::lock_guard lock{ m_mutex };
            if (!rewind()) {
                return boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
            }
            std::ofstream out{ destination, std::ios::binary | std::ios::trunc };
            if (!out.is_open()) {
                return boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
            std::vector<char> buffer(256 * 1024);
            auto remaining{ m_size };
            while (remaining > 0) {
                auto const wanted{ static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining)) };
                auto const got{ readSome(buffer.data(), static_cast<std::size_t>(wanted)) };
                if (got <= 0) {
                    return boost::system::errc::make_error_code(boost::system::errc::io_error);
                }
                out.write(buffer.data(), got);
                if (!out) {
                    return boost::system::errc::make_error_code(boost::system::errc::io_error);
                }
                remaining -= static_cast<std::uint64_t>(got);
            }
            out.close();
            if (out.fail()) {
                return boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
            return {};
        }

    private:
        /**
         * @brief Read from the live body handle without depending on its directory entry.
         */
        auto readSome(char* data, std::size_t size) const -> std::size_t {
#if defined(_WIN32)
            DWORD got{ 0 };
            auto const wanted{ static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD)) };
            return ::ReadFile(m_file, data, wanted, &got, nullptr) ? static_cast<std::size_t>(got) : 0;
#else
            m_file.read(data, static_cast<std::streamsize>(size));
            return static_cast<std::size_t>(m_file.gcount());
#endif
        }

        auto rewind() const -> bool {
#if defined(_WIN32)
            LARGE_INTEGER position{};
            return IsOpen() && ::SetFilePointerEx(m_file, position, nullptr, FILE_BEGIN);
#else
            if (!m_file.is_open()) {
                return false;
            }
            m_file.clear();
            m_file.seekg(0, std::ios::beg);
            return static_cast<bool>(m_file);
#endif
        }
    };

    /**
     * @brief Best-effort exclusive lock on a file, held for the cache's lifetime.
     */
    class CacheLock {
    private:
#if defined(_WIN32)
        HANDLE m_handle{ INVALID_HANDLE_VALUE }; ///< Locked file.
#else
        int m_fd{ -1 }; ///< Locked file.
#endif

    public:
        explicit CacheLock(std::filesystem::path const& path) {
#if defined(_WIN32)
            auto const handle{ ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (handle == INVALID_HANDLE_VALUE) {
                return;
            }
            OVERLAPPED overlapped{};
            if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
                m_handle = handle;
            } else {
                ::CloseHandle(handle);
            }
#else
            auto const fd{ ::open(path.c_str(), O_RDWR | O_CREAT, 0644) };
            if (fd < 0) {
                return;
            }
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                m_fd = fd;
            } else {
                ::close(fd);
            }
#endif
        }

        CacheLock(CacheLock const&) = delete;
        auto operator=(CacheLock const&) -> CacheLock& = delete;

        ~CacheLock() {
#if defined(_WIN32)
            if (m_handle != INVALID_HANDLE_VALUE) {
                OVERLAPPED overlapped{};
                ::UnlockFileEx(m_handle, 0, 1, 0, &overlapped);
                ::CloseHandle(m_handle);
            }
#else
            if (m_fd >= 0) {
                ::flock(m_fd, LOCK_UN);
                ::close(m_fd);
            }
#endif
        }

        [[nodiscard]] auto Acquired() const -> bool {
#if defined(_WIN32)
            return m_handle != INVALID_HANDLE_VALUE;
#else
            return m_fd >= 0;
#endif
        }
    };

} // namespace mhttp::detail

namespace mhttp::client {

    /**
     * @brief State behind DiskCache; every member is guarded by m_mutex.
     */
    class DiskCache::Impl {
    private:
        std::filesystem::path m_directory;                                                        ///< Cache directory.
        std::uint64_t m_max_size{ 512ULL * 1024 * 1024 };                                         ///< Size budget; 0 = unlimited.
        std::chrono::seconds m_max_age{ std::chrono::hours{ 24 * 7 } };                           ///< Default lifetime; 0 = unlimited.
        std::chrono::steady_clock::time_point m_last_cleanup{ std::chrono::steady_clock::now() }; ///< Throttles Get() cleanups.
        std::unique_ptr<detail::CacheLock> m_lock;                                                ///< Cross-process guard.
        mutable std::mutex m_mutex;                                                               ///< Guards everything.

    public:
        explicit Impl(std::filesystem::path directory) : m_directory{ std::move(directory) } {
            ensureDirectory();
            m_lock = std::make_unique<detail::CacheLock>(m_directory / detail::CACHE_LOCK_FILE);
            if (!m_lock->Acquired()) {
                throw std::runtime_error{ "cannot lock disk cache directory" };
            }
        }

        auto Get(std::string_view key) -> std::unique_ptr<Entry> {
            std::lock_guard lock{ m_mutex };
            ensureDirectory();
            if (auto const now{ std::chrono::steady_clock::now() }; now - m_last_cleanup > std::chrono::seconds{ 60 }) {
                m_last_cleanup = now;
                cleanupLocked();
            }
            auto const dir{ entryDir(key) };
            auto const body{ dir / detail::CACHE_BODY_FILE };
            std::error_code ec;
            if (!std::filesystem::exists(body, ec)) {
                return nullptr;
            }
            if (!isCurrentEntryFor(dir, key)) {
                // Foreign layout version, or another key with the same hash: never serve it.
                detail::remove_tree(dir);
                return nullptr;
            }
            auto const size{ std::filesystem::file_size(body, ec) };
            if (ec) {
                return nullptr;
            }
            auto const expires{ readExpires(dir) };
            if (isExpired(body, expires)) {
                detail::remove_tree(dir);
                return nullptr;
            }
            auto entry{ std::make_unique<detail::DiskEntry>(body, static_cast<std::uint64_t>(size), detail::read_text_file(dir / detail::CACHE_META_FILE).value_or(""), expires) };
            if (!entry->IsOpen()) {
                return nullptr;
            }
            touch(body);
            return entry;
        }

        auto Put(
            std::string_view key,
            std::filesystem::path const& source_body,
            std::string_view metadata,
            std::optional<TimePoint> expires_at
        ) -> void {
            std::lock_guard lock{ m_mutex };
            ensureDirectory();
            std::error_code ec;
            if (!std::filesystem::is_regular_file(source_body, ec)) {
                return;
            }
            auto const dir{ entryDir(key) };
            // Stage the whole entry, then swap it in: readers see the old or the new entry,
            // never a half-written body/meta pair.
            auto staging{ dir };
            staging += ".tmp";
            std::filesystem::create_directories(dir.parent_path(), ec);
            detail::remove_tree(staging);
            std::filesystem::create_directories(staging, ec);
            if (ec) {
                return;
            }
            std::filesystem::copy_file(source_body, staging / detail::CACHE_BODY_FILE, std::filesystem::copy_options::overwrite_existing, ec);
            bool written{ !ec };
            written = written && detail::write_text_file(staging / detail::CACHE_META_FILE, metadata);
            written = written && detail::write_text_file(staging / detail::CACHE_KEY_FILE, key);
            written = written && detail::write_text_file(staging / detail::CACHE_VERSION_FILE, std::to_string(detail::CACHE_FORMAT_VERSION));
            if (expires_at) {
                written = written && detail::write_text_file(staging / detail::CACHE_EXPIRES_FILE, std::to_string(detail::to_unix_seconds(*expires_at)));
            }
            if (!written) {
                detail::remove_tree(staging);
                return;
            }
            detail::remove_tree(dir);
            std::filesystem::rename(staging, dir, ec);
            if (ec) {
                detail::remove_tree(staging);
                return;
            }
            cleanupLocked();
        }

        auto UpdateMetadata(
            std::string_view key,
            std::string_view metadata,
            std::optional<TimePoint> expires_at
        ) -> bool {
            std::lock_guard lock{ m_mutex };
            auto const dir{ entryDir(key) };
            auto const body{ dir / detail::CACHE_BODY_FILE };
            std::error_code ec;
            if (!std::filesystem::exists(body, ec) || !isCurrentEntryFor(dir, key)) {
                return false;
            }
            if (isExpired(body, readExpires(dir))) {
                detail::remove_tree(dir);
                return false;
            }
            if (!replaceFile(dir / detail::CACHE_META_FILE, metadata)) {
                return false;
            }
            if (expires_at) {
                replaceFile(dir / detail::CACHE_EXPIRES_FILE, std::to_string(detail::to_unix_seconds(*expires_at)));
            } else {
                std::filesystem::remove(dir / detail::CACHE_EXPIRES_FILE, ec);
            }
            touch(body);
            return true;
        }

        auto Remove(std::string_view key) -> void {
            std::lock_guard lock{ m_mutex };
            detail::remove_tree(entryDir(key));
        }

        auto Clear() -> void {
            std::lock_guard lock{ m_mutex };
            std::error_code ec;
            for (auto const& child : std::filesystem::directory_iterator{ m_directory, ec }) {
                if (child.path().filename() != detail::CACHE_LOCK_FILE) {
                    detail::remove_tree(child.path());
                }
            }
        }

        auto Cleanup() -> void {
            std::lock_guard lock{ m_mutex };
            ensureDirectory();
            cleanupLocked();
        }

        auto SetMaxSize(std::uint64_t max_bytes) -> void {
            std::lock_guard lock{ m_mutex };
            m_max_size = max_bytes;
        }

        auto SetMaxAge(std::chrono::seconds max_age) -> void {
            std::lock_guard lock{ m_mutex };
            m_max_age = max_age;
        }

        [[nodiscard]] auto TotalSize() const -> std::uint64_t {
            std::lock_guard lock{ m_mutex };
            std::uint64_t total{ 0 };
            for (auto const& dir : detail::collect_entry_dirs(m_directory)) {
                std::error_code ec;
                auto const size{ std::filesystem::file_size(dir / detail::CACHE_BODY_FILE, ec) };
                total += ec ? 0 : static_cast<std::uint64_t>(size);
            }
            return total;
        }

        [[nodiscard]] auto EntryCount() const -> std::size_t {
            std::lock_guard lock{ m_mutex };
            return static_cast<std::size_t>(std::ranges::count_if(detail::collect_entry_dirs(m_directory), [](auto const& dir) {
                std::error_code ec;
                return std::filesystem::exists(dir / detail::CACHE_BODY_FILE, ec);
            }));
        }

        [[nodiscard]] auto Directory() const -> std::filesystem::path const& {
            return m_directory;
        }

    private:
        auto ensureDirectory() const -> void {
            std::error_code ec;
            std::filesystem::create_directories(m_directory, ec);
        }

        [[nodiscard]] auto entryDir(std::string_view key) const -> std::filesystem::path {
            auto const hash{ detail::fnv1a_hex(key) };
            return m_directory / hash.substr(0, 2) / hash;
        }

        /**
         * @brief Check the layout version and that the entry belongs to `key`.
         * @return True when the entry may be served for `key`.
         */
        [[nodiscard]] static auto isCurrentEntryFor(std::filesystem::path const& dir, std::string_view key) -> bool {
            return detail::parse_int(detail::read_text_file(dir / detail::CACHE_VERSION_FILE)) == detail::CACHE_FORMAT_VERSION && detail::read_text_file(dir / detail::CACHE_KEY_FILE) == key;
        }

        [[nodiscard]] static auto readExpires(std::filesystem::path const& dir) -> std::optional<TimePoint> {
            auto const seconds{ detail::parse_int(detail::read_text_file(dir / detail::CACHE_EXPIRES_FILE)) };
            if (!seconds) {
                return std::nullopt;
            }
            return TimePoint{ std::chrono::seconds{ *seconds } };
        }

        /**
         * @brief Decide expiry: the per-entry deadline, else max_age since the last use.
         * @param body Body file; its mtime is the last use.
         * @param expires Per-entry deadline.
         * @return True when expired.
         */
        [[nodiscard]] auto isExpired(
            std::filesystem::path const& body,
            std::optional<TimePoint> const& expires
        ) const -> bool {
            auto const now{ Clock::now() };
            if (expires) {
                return now >= *expires;
            }
            if (m_max_age.count() <= 0) {
                return false;
            }
            std::error_code ec;
            auto const written{ std::filesystem::last_write_time(body, ec) };
            if (ec) {
                return false;
            }
            return now - std::chrono::clock_cast<Clock>(written) > m_max_age;
        }

        static auto touch(std::filesystem::path const& body) -> void {
            std::error_code ec;
            std::filesystem::last_write_time(body, std::filesystem::file_time_type::clock::now(), ec);
        }

        static auto replaceFile(std::filesystem::path const& path, std::string_view data) -> bool {
            auto staging{ path };
            staging += ".tmp";
            std::error_code ec;
            if (!detail::write_text_file(staging, data)) {
                std::filesystem::remove(staging, ec);
                return false;
            }
            std::filesystem::remove(path, ec);
            std::filesystem::rename(staging, path, ec);
            if (ec) {
                std::filesystem::remove(staging, ec);
                return false;
            }
            return true;
        }

        /**
         * @brief Purge staging leftovers, invalid and expired entries, then trim to the budget.
         */
        auto cleanupLocked() -> void {
            std::error_code ec;
            for (auto const& shard : std::filesystem::directory_iterator{ m_directory, ec }) {
                std::error_code shard_ec;
                for (auto const& leaf : std::filesystem::directory_iterator{ shard.path(), shard_ec }) {
                    if (leaf.path().filename().string().ends_with(".tmp")) {
                        detail::remove_tree(leaf.path());
                    }
                }
            }

            struct Info {
                std::filesystem::path dir;
                std::uint64_t size{ 0 };
                std::filesystem::file_time_type used;
            };

            std::vector<Info> entries;
            std::uint64_t total{ 0 };
            for (auto const& dir : detail::collect_entry_dirs(m_directory)) {
                auto const body{ dir / detail::CACHE_BODY_FILE };
                std::error_code body_ec;
                auto const size{ std::filesystem::file_size(body, body_ec) };
                if (body_ec || detail::parse_int(detail::read_text_file(dir / detail::CACHE_VERSION_FILE)) != detail::CACHE_FORMAT_VERSION || isExpired(body, readExpires(dir))) {
                    detail::remove_tree(dir);
                    continue;
                }
                auto const used{ std::filesystem::last_write_time(body, body_ec) };
                entries.push_back(Info{ dir, static_cast<std::uint64_t>(size), used });
                total += static_cast<std::uint64_t>(size);
            }
            if (m_max_size == 0 || total <= m_max_size) {
                return;
            }
            std::ranges::sort(entries, {}, &Info::used);
            for (auto const& entry : entries) {
                if (total <= m_max_size) {
                    break;
                }
                detail::remove_tree(entry.dir);
                std::error_code exists_ec;
                if (!std::filesystem::exists(entry.dir, exists_ec)) {
                    total -= std::min(total, entry.size);
                }
            }
        }
    };

    DiskCache::DiskCache(std::filesystem::path directory) : m_impl{ std::make_unique<Impl>(std::move(directory)) } {}

    DiskCache::~DiskCache() = default;

    auto DiskCache::Get(std::string_view key) -> std::unique_ptr<Entry> {
        return m_impl->Get(key);
    }

    auto DiskCache::Put(
        std::string_view key,
        std::filesystem::path const& source_body,
        std::string_view metadata,
        std::optional<TimePoint> expires_at
    ) -> void {
        m_impl->Put(key, source_body, metadata, expires_at);
    }

    auto DiskCache::UpdateMetadata(
        std::string_view key,
        std::string_view metadata,
        std::optional<TimePoint> expires_at
    ) -> bool {
        return m_impl->UpdateMetadata(key, metadata, expires_at);
    }

    auto DiskCache::Remove(std::string_view key) -> void {
        m_impl->Remove(key);
    }

    auto DiskCache::Clear() -> void {
        m_impl->Clear();
    }

    auto DiskCache::Cleanup() -> void {
        m_impl->Cleanup();
    }

    auto DiskCache::SetMaxSize(std::uint64_t max_bytes) -> void {
        m_impl->SetMaxSize(max_bytes);
    }

    auto DiskCache::SetMaxAge(std::chrono::seconds max_age) -> void {
        m_impl->SetMaxAge(max_age);
    }

    auto DiskCache::TotalSize() const -> std::uint64_t {
        return m_impl->TotalSize();
    }

    auto DiskCache::EntryCount() const -> std::size_t {
        return m_impl->EntryCount();
    }

    auto DiskCache::Directory() const -> std::filesystem::path const& {
        return m_impl->Directory();
    }

} // namespace mhttp::client
