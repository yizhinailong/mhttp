/**
 * @file static_file.cppm
 * @brief Open rooted regular files without following links and retain native handles through bounded reads.
 */
module;

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

module mhttp:server.impl.static_file;

import std;
import :core.error;
// Materialize owned system_error copy metadata before filesystem path conversion can throw.
import :core.impl.io_error;
import :core.status;

namespace mhttp::impl {

    /**
     * @brief Close one native file or directory on every completion and exception path.
     * @note Instances are move-only and are used on worker threads, never for blocking I/O on I/O threads.
     */
    class StaticHandle {
    public:
#ifdef _WIN32
        using Native = HANDLE; ///< Windows handle type confined to this implementation partition.
        static inline Native const INVALID{ INVALID_HANDLE_VALUE }; ///< Invalid Windows file handle.
#else
        using Native = int; ///< POSIX descriptor confined to this implementation partition.
        static constexpr Native INVALID{ -1 }; ///< Invalid POSIX descriptor.
#endif
    private:
        Native m_value{ INVALID }; ///< Sole owned native resource.
    public:
        /**
         * @brief Adopt one native resource, including an invalid failed-open result.
         * @param value Handle whose ownership is transferred.
         */
        explicit StaticHandle(Native value = INVALID) : m_value{ value } {}
        /**
         * @brief Transfer native ownership without closing the resource.
         * @param other Source left invalid.
         */
        StaticHandle(StaticHandle&& other) noexcept : m_value{ std::exchange(other.m_value, INVALID) } {}
        /**
         * @brief Prevent two owners from closing the same native resource.
         * @param other Unused source.
         */
        StaticHandle(StaticHandle const& other) = delete;
        /**
         * @brief Release the owned handle if present.
         */
        ~StaticHandle() {
            if (m_value != INVALID) {
#ifdef _WIN32
                static_cast<void>(::CloseHandle(m_value));
#else
                static_cast<void>(::close(m_value));
#endif
            }
        }
        /**
         * @brief Borrow the handle while its owner remains alive.
         * @return Native handle or INVALID.
         */
        [[nodiscard]] auto Get() const noexcept -> Native { return m_value; }
    };

    /**
     * @brief Own metadata collected from the opened file rather than a separately resolved pathname.
     */
    struct StaticMetadata {
        std::uint64_t size{}; ///< Exact representation length.
        std::chrono::sys_seconds modified; ///< Last modification rounded down to UTC seconds.
        std::string identity; ///< Weak validator and mutation detector including native identity and timestamps.
        bool directory{}; ///< Directory metadata is used only while resolving an index.
    };

    /**
     * @brief Inspect an already-open native handle on a worker thread.
     * @param handle Borrowed file or directory handle.
     * @return Metadata, or no value for unsupported objects, links, or native query failures.
     */
    auto static_metadata(StaticHandle::Native handle) -> std::optional<StaticMetadata> {
#ifdef _WIN32
        BY_HANDLE_FILE_INFORMATION info{};
        if (::GetFileType(handle) != FILE_TYPE_DISK || !::GetFileInformationByHandle(handle, &info)
            || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) { return std::nullopt; }
        auto const size = (std::uint64_t{ info.nFileSizeHigh } << 32) | info.nFileSizeLow;
        auto const ticks = (std::uint64_t{ info.ftLastWriteTime.dwHighDateTime } << 32) | info.ftLastWriteTime.dwLowDateTime;
        auto const id = (std::uint64_t{ info.nFileIndexHigh } << 32) | info.nFileIndexLow;
        return StaticMetadata{ size, std::chrono::sys_seconds{ std::chrono::seconds{
            static_cast<std::int64_t>(ticks / 10000000) - 11644473600LL } },
            std::format("{:x}-{:x}-{:x}-{:x}", info.dwVolumeSerialNumber, id, size, ticks),
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 };
#else
        struct stat info{};
        if (::fstat(handle, &info) != 0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode)) || info.st_size < 0) {
            return std::nullopt;
        }
#ifdef __APPLE__
        auto const modified = info.st_mtimespec;
        auto const changed = info.st_ctimespec;
#else
        auto const modified = info.st_mtim;
        auto const changed = info.st_ctim;
#endif
        return StaticMetadata{ static_cast<std::uint64_t>(info.st_size),
            std::chrono::sys_seconds{ std::chrono::seconds{ modified.tv_sec } },
            std::format("{:x}-{:x}-{:x}-{:x}-{:x}-{:x}-{:x}", info.st_dev, info.st_ino, info.st_size,
                modified.tv_sec, modified.tv_nsec, changed.tv_sec, changed.tv_nsec), S_ISDIR(info.st_mode) };
#endif
    }

#ifdef _WIN32
    /**
     * @brief Query the fully resolved name of the actual open handle for containment verification.
     * @param handle Borrowed directory or file, pinned against rename by its sharing mode.
     * @return Normalized absolute native path, or no value if the query fails.
     */
    auto static_final_path(HANDLE handle) -> std::optional<std::wstring> {
        auto const size = ::GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
        if (size == 0) { return std::nullopt; }
        std::wstring path(size, L'\0');
        auto const written = ::GetFinalPathNameByHandleW(handle, path.data(), size, FILE_NAME_NORMALIZED);
        if (written == 0 || written >= size) { return std::nullopt; }
        path.resize(written);
        return path;
    }
#endif

    /**
     * @brief Retain one opened regular file independently of its mount and stream callbacks.
     */
    class StaticFile {
    private:
        StaticHandle m_handle; ///< Sole native file owner.
        StaticMetadata m_metadata; ///< Original identity and exact size of the selected representation.
        std::string m_name; ///< Actual selected filename, including a directory index when used.
    public:
        /**
         * @brief Adopt an opened regular file and its handle-derived metadata.
         * @param handle Sole native resource owner.
         * @param metadata Owned immutable selection metadata.
         * @param name Owned final filename for media lookup.
         */
        StaticFile(StaticHandle handle, StaticMetadata metadata, std::string name)
            : m_handle{ std::move(handle) }, m_metadata{ std::move(metadata) }, m_name{ std::move(name) } {}
        /**
         * @brief Borrow the actual selected filename without further filesystem access.
         * @return Owned final component while this file lives.
         */
        [[nodiscard]] auto Name() const noexcept -> std::string_view { return m_name; }
        /**
         * @brief Borrow immutable metadata while the file owner lives.
         * @return Original selected representation metadata.
         */
        [[nodiscard]] auto Metadata() const noexcept -> StaticMetadata const& { return m_metadata; }
        /**
         * @brief Read an exact bounded segment from the retained handle on a worker thread.
         * @param offset Validated absolute byte offset.
         * @param count Bounded positive byte count within the original file size.
         * @return Owned bytes or Read if the file changed, was truncated, or the native read failed.
         * @note Call sequentially. Metadata is checked before and after reading; no pathname is reopened.
         */
        auto Read(std::uint64_t offset, std::size_t count) -> Result<std::string> {
            auto before = static_metadata(m_handle.Get());
            if (!before || before->identity != m_metadata.identity || offset > m_metadata.size
                || count > m_metadata.size - offset) { return std::unexpected{ Error::Read }; }
            std::string bytes(count, '\0');
            std::size_t done{};
#ifdef _WIN32
            LARGE_INTEGER position{};
            position.QuadPart = static_cast<LONGLONG>(offset);
            if (!::SetFilePointerEx(m_handle.Get(), position, nullptr, FILE_BEGIN)) { return std::unexpected{ Error::Read }; }
#endif
            while (done < count) {
#ifdef _WIN32
                DWORD read{};
                auto const size = static_cast<DWORD>(std::min<std::size_t>(count - done, 0x7fffffff));
                if (!::ReadFile(m_handle.Get(), bytes.data() + done, size, &read, nullptr) || read == 0) {
                    return std::unexpected{ Error::Read };
                }
#else
                auto const read = ::pread(m_handle.Get(), bytes.data() + done, count - done, static_cast<off_t>(offset + done));
                if (read < 0 && errno == EINTR) { continue; }
                if (read <= 0) { return std::unexpected{ Error::Read }; }
#endif
                done += static_cast<std::size_t>(read);
            }
            auto after = static_metadata(m_handle.Get());
            if (!after || after->identity != m_metadata.identity) { return std::unexpected{ Error::Read }; }
            return bytes;
        }
    };

    /**
     * @brief Pin a mount root and resolve only ordinary child components without following links.
     * @note Construct during configuration. Open and Read are blocking operations for Runtime workers.
     */
    class StaticRoot {
    private:
        StaticHandle m_handle; ///< Open root directory; POSIX resolution is relative to this descriptor.
        std::filesystem::path m_path; ///< Normalized root path used for native Windows opens.
        /**
         * @brief Adopt a validated root without making further filesystem calls.
         * @param handle Owned directory.
         * @param path Owned normalized root path.
         */
        StaticRoot(StaticHandle handle, std::filesystem::path path)
            : m_handle{ std::move(handle) }, m_path{ std::move(path) } {}
    public:
        /**
         * @brief Open a trusted configured root, permitting the root itself to resolve through links.
         * @param path Borrowed native directory path, copied during this call.
         * @return Owned pinned directory or InvalidRequest for an unavailable/non-directory root.
         */
        static auto Create(
            std::filesystem::path const& path
        ) -> Result<std::shared_ptr<StaticRoot>> {
#ifdef _WIN32
            StaticHandle handle{ ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
            if (handle.Get() == StaticHandle::INVALID) { return std::unexpected{ Error::InvalidRequest }; }
            auto normalized = static_final_path(handle.Get());
            if (!normalized) { return std::unexpected{ Error::InvalidRequest }; }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!::GetFileInformationByHandle(handle.Get(), &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return std::unexpected{ Error::InvalidRequest };
            }
            return std::shared_ptr<StaticRoot>{ new StaticRoot{ std::move(handle), std::filesystem::path{ *normalized } } };
#else
            StaticHandle handle{ ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
            if (handle.Get() == StaticHandle::INVALID) { return std::unexpected{ Error::InvalidRequest }; }
            return std::shared_ptr<StaticRoot>{ new StaticRoot{ std::move(handle), {} } };
#endif
        }
        /**
         * @brief Open a regular descendant using validated single-component UTF-8 names.
         * @param components Owned portable components without separators, dot segments, or device names.
         * @param index Validated optional single filename appended when the selected object is a directory.
         * @return Owned opened file, or NotFound for inaccessible, missing, linked, or unsupported objects.
         * @note Windows pins every traversed directory against rename and verifies the opened file's final
         *       name against the root handle. POSIX walks descriptors with openat and O_NOFOLLOW.
         */
        auto Open(
            std::vector<std::string> components,
            std::string const& index
        ) const -> std::expected<std::shared_ptr<StaticFile>, Status> {
            bool indexed{};
            if (components.empty()) {
                if (index.empty()) { return std::unexpected{ Status::NotFound }; }
                components.push_back(index);
                indexed = true;
            }
            std::vector<StaticHandle> directories;
            auto parent = m_handle.Get();
            auto path = m_path;
            for (std::size_t i{}; i < components.size(); ++i) {
#ifdef _WIN32
                auto const& name = components[i];
                auto component = std::filesystem::path{ std::u8string{
                    reinterpret_cast<char8_t const*>(name.data()), name.size() } };
                path /= component;
                StaticHandle handle{ ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
#else
                StaticHandle handle{ ::openat(parent, components[i].c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) };
#endif
                if (handle.Get() == StaticHandle::INVALID) { return std::unexpected{ Status::NotFound }; }
                auto metadata = static_metadata(handle.Get());
                if (!metadata) { return std::unexpected{ Status::NotFound }; }
                if (metadata->directory) {
                    if (i + 1 == components.size()) {
                        if (index.empty() || indexed) { return std::unexpected{ Status::NotFound }; }
                        components.push_back(index);
                        indexed = true;
                    }
                    parent = handle.Get();
                    directories.push_back(std::move(handle));
                    continue;
                }
                if (i + 1 != components.size()) { return std::unexpected{ Status::NotFound }; }
#ifdef _WIN32
                auto root_name = static_final_path(m_handle.Get());
                auto file_name = static_final_path(handle.Get());
                if (!root_name || !file_name) { return std::unexpected{ Status::NotFound }; }
                if (!root_name->ends_with(L'\\')) { root_name->push_back(L'\\'); }
                if (!file_name->starts_with(*root_name)) { return std::unexpected{ Status::NotFound }; }
#endif
                return std::make_shared<StaticFile>(std::move(handle), std::move(*metadata), components[i]);
            }
            return std::unexpected{ Status::NotFound };
        }
    };

} // namespace mhttp::impl
