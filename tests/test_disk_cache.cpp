/**
 * @file test_disk_cache.cpp
 * @brief Exercise persistent blobs, live handles, expiry, eviction, and cache ownership.
 */
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

import std;
import mhttp;

#include "support.hpp"

using namespace test_support;
using mhttp::client::Cache;
using mhttp::client::DiskCache;

namespace {

    /**
     * @brief Verify persistence, opaque metadata, replacement, and live handles after removal.
     */
    auto test_blobs() -> void {
        TempDir dir;
        auto const source{ dir.Write("source.bin", std::string{ "a\0b", 3 }) };
        auto const directory{ dir.path / "cache" };
        {
            DiskCache cache{ directory };
            cache.Put("first", source, "opaque metadata", std::nullopt);
            check(cache.EntryCount() == 1 && cache.TotalSize() == 3, "cache statistics must count body bytes");
            auto entry{ cache.Get("first") };
            if (!check(entry && entry->ReadAll() == std::string{ "a\0b", 3 }, "binary bodies must round-trip")) {
                return;
            }
            check(entry->Metadata() == "opaque metadata" && !entry->ExpiresAt(), "metadata and optional deadlines must round-trip");
            check(entry->ReadAll() == std::string{ "a\0b", 3 }, "entry reads must rewind their cursor");
            check(!entry->CopyToFile(dir.path / "copy.bin") && std::filesystem::file_size(dir.path / "copy.bin") == 3, "copying a live body must preserve its bytes");

            cache.Put("first", dir.path / "missing", "must not replace", std::nullopt);
            check(cache.Get("first")->Metadata() == "opaque metadata", "a missing source must preserve the existing entry");
            dir.Write("source.bin", "replacement");
            cache.Put("first", source, "new metadata", std::nullopt);
            auto current{ cache.Get("first") };
            check(current && current->ReadAll() == "replacement", "replacement must publish the new body while old handles are alive");
            check(entry->ReadAll() == std::string{ "a\0b", 3 }, "a live handle must continue reading its original body after replacement");
            check(cache.UpdateMetadata("first", "updated", std::nullopt), "metadata of live entries must be replaceable");
            check(cache.Get("first")->Metadata() == "updated", "metadata updates must be visible to later readers");
            check(!cache.UpdateMetadata("absent", "x", std::nullopt), "updating an absent key must fail");
            cache.Remove("first");
            check(!cache.Get("first") && cache.EntryCount() == 0, "removal must hide the entry even with live handles");
            check(current && current->ReadAll() == "replacement", "removal must preserve already handed-out bodies");
            cache.Put("persistent", source, "saved", std::nullopt);
        }
        DiskCache reopened{ directory };
        auto const entry{ reopened.Get("persistent") };
        check(entry && entry->ReadAll() == "replacement" && entry->Metadata() == "saved", "a reopened cache must retain stored bodies and metadata");
        reopened.Clear();
        check(reopened.EntryCount() == 0 && reopened.TotalSize() == 0 && !reopened.Get("persistent"), "Clear must remove all entries");
    }

    /**
     * @brief Verify deadlines, default age, and the least recently used size budget.
     */
    auto test_expiry_and_eviction() -> void {
        TempDir dir;
        auto const source{ dir.Write("source", "1234") };
        DiskCache cache{ dir.path / "cache" };
        auto const expired{ Cache::Clock::now() - std::chrono::seconds{ 5 } };
        cache.Put("expired", source, "", expired);
        check(!cache.Get("expired"), "past deadlines must expire immediately");
        cache.Put("live", source, "", Cache::Clock::now() + std::chrono::hours{ 1 });
        check(cache.Get("live") != nullptr, "future deadlines must remain readable");
        check(cache.UpdateMetadata("live", "", expired) && !cache.Get("live"), "metadata updates must apply a new deadline");
        cache.SetMaxAge(std::chrono::seconds{ 1 });
        cache.Put("aged", source, "", std::nullopt);
        for (auto const& item : std::filesystem::recursive_directory_iterator{ cache.Directory() }) {
            if (item.path().filename() == "body") {
                std::filesystem::last_write_time(item.path(), std::filesystem::file_time_type::clock::now() - std::chrono::hours{ 1 });
            }
        }
        check(!cache.Get("aged"), "default age must expire unused entries");
        cache.SetMaxAge(std::chrono::seconds{ 0 });
        cache.SetMaxSize(8);
        cache.Put("old", source, "", std::nullopt);
        for (auto const& item : std::filesystem::recursive_directory_iterator{ cache.Directory() }) {
            if (item.path().filename() == "body") {
                std::filesystem::last_write_time(item.path(), std::filesystem::file_time_type::clock::now() - std::chrono::hours{ 1 });
            }
        }
        cache.Put("recent", source, "", std::nullopt);
        cache.Put("newest", source, "", std::nullopt);
        check(!cache.Get("old") && cache.Get("recent") && cache.Get("newest") && cache.TotalSize() == 8, "over-budget insertion must evict the oldest entry");
    }

    /**
     * @brief Reject concurrent cache owners and foreign entry metadata.
     */
    auto test_ownership_and_corruption() -> void {
        TempDir dir;
        auto const source{ dir.Write("source", "data") };
        DiskCache cache{ dir.path / "cache" };
        bool rejected{ false };
        try {
            DiskCache second{ cache.Directory() };
        } catch (std::runtime_error const&) {
            rejected = true;
        }
        check(rejected, "a second cache owner must fail before touching the directory");
        cache.Put("key", source, "", std::nullopt);
        for (auto const& item : std::filesystem::recursive_directory_iterator{ cache.Directory() }) {
            if (item.path().filename() == "key") {
                std::ofstream file{ item.path(), std::ios::trunc };
                file << "another key";
            }
        }
        check(!cache.Get("key"), "a hash collision or corrupt key file must not serve another key's data");
        cache.Put("key", source, "", std::nullopt);
        for (auto const& item : std::filesystem::recursive_directory_iterator{ cache.Directory() }) {
            if (item.path().filename() == "version") {
                std::ofstream file{ item.path(), std::ios::trunc };
                file << "999";
            }
        }
        cache.Cleanup();
        check(cache.EntryCount() == 0, "cleanup must remove foreign layout versions");
    }

} // namespace

auto main(int argc, char** argv) -> int {
    init(argc, argv);
    run_case("blobs and persistence", test_blobs);
    run_case("expiry and eviction", test_expiry_and_eviction);
    run_case("ownership and corruption", test_ownership_and_corruption);
    return finish("test_disk_cache");
}
