/**
 * @file http_date.cppm
 * @brief Format HTTP dates in UTC without shared C calendar state or locale dependencies.
 */
export module mhttp:utils.date;

import std;

export namespace mhttp {

    /**
     * @brief Format a system-clock instant as an IMF-fixdate value in GMT.
     * @param time Instant to format; subsecond precision is discarded toward the earlier second.
     * @return Owned English date in the form Sun, 06 Nov 1994 08:49:37 GMT.
     * @throws std::out_of_range If the calendar year cannot be written with four decimal digits.
     * @note Calendar conversion is independent of the process locale and time zone and is thread-safe.
     */
    [[nodiscard]] auto FormatHttpDate(std::chrono::system_clock::time_point time) -> std::string {
        using namespace std::chrono;
        constexpr std::array<std::string_view, 7> WEEKDAYS{
            "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
        };
        constexpr std::array<std::string_view, 12> MONTHS{
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        auto const day = floor<days>(time);
        if (day < sys_days{ year{ 0 } / January / 1 } || day >= sys_days{ year{ 10000 } / January / 1 }) {
            throw std::out_of_range{ "HTTP date requires a four-digit year" };
        }
        year_month_day const date{ day };
        hh_mm_ss const clock{ floor<seconds>(time) - day };
        return std::format("{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT",
            WEEKDAYS[weekday{ day }.c_encoding()], static_cast<unsigned>(date.day()),
            MONTHS[static_cast<unsigned>(date.month()) - 1], static_cast<int>(date.year()),
            clock.hours().count(), clock.minutes().count(), clock.seconds().count());
    }

} // namespace mhttp
