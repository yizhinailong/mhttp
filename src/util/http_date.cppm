/**
 * @file http_date.cppm
 * @brief Format HTTP dates, file timestamps, and random multipart boundaries.
 */
export module mhttp:http_date;

import std;

namespace mhttp::detail {

    /**
     * @brief Format a time as an IMF-fixdate (RFC 9110), e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
     * @param time Seconds since the Unix epoch.
     * @return Formatted date.
     */
    auto format_http_gmt_date(std::time_t time) -> std::string {
        std::chrono::sys_seconds const point{ std::chrono::seconds{ time } };
        return std::format("{:%a, %d %b %Y %H:%M:%S} GMT", point);
    }

    /**
     * @brief Return the current time as an HTTP date, cached per thread for one second.
     * @return Formatted current date.
     */
    auto current_gmt_date() -> std::string {
        thread_local std::time_t s_last_time{ 0 };
        thread_local std::string s_cache;
        auto const now{ std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) };
        if (now != s_last_time) {
            s_cache = format_http_gmt_date(now);
            s_last_time = now;
        }
        return s_cache;
    }

    /**
     * @brief Return a file's last write time in seconds since the Unix epoch.
     * @param path File to inspect.
     * @param ec Receives the filesystem error.
     * @return Last write time, or 0 on error.
     */
    auto file_last_write_time(
        std::filesystem::path const& path,
        std::error_code& ec
    ) -> std::time_t {
        auto const file_time{ std::filesystem::last_write_time(path, ec) };
        if (ec) {
            return 0;
        }
        auto const system_time{ std::chrono::clock_cast<std::chrono::system_clock>(file_time) };
        return std::chrono::system_clock::to_time_t(std::chrono::floor<std::chrono::seconds>(system_time));
    }

    /**
     * @brief Format a time as "MM-DD-YYYY HH:MM" in UTC for directory listings.
     * @param time Seconds since the Unix epoch.
     * @return Formatted time.
     */
    auto format_listing_time(std::time_t time) -> std::string {
        std::chrono::sys_seconds const point{ std::chrono::seconds{ time } };
        return std::format("{:%m-%d-%Y %H:%M}", point);
    }

    /**
     * @brief Generate random hex text.
     * @param length Number of hex digits.
     * @return Random lowercase hex string.
     */
    auto random_hex(std::size_t length) -> std::string {
        thread_local std::mt19937_64 s_generator{ std::random_device{}() };
        static constexpr std::string_view DIGITS{ "0123456789abcdef" };
        std::uniform_int_distribution<std::size_t> distribution{ 0, DIGITS.size() - 1 };
        std::string out(length, '0');
        for (auto& c : out) {
            c = DIGITS[distribution(s_generator)];
        }
        return out;
    }

    /**
     * @brief Generate a multipart boundary unlikely to appear in the payload.
     * @return Boundary token without the leading "--".
     */
    auto generate_boundary() -> std::string {
        return "----------------mhttp" + random_hex(24);
    }

} // namespace mhttp::detail
