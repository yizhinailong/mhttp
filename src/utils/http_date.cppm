/**
 * @file http_date.cppm
 * @brief Format HTTP dates in UTC without shared C calendar state or locale dependencies.
 */
export module mhttp:utils.date;

import std;

export namespace mhttp {

    /**
     * @brief Parse IMF-fixdate and the two obsolete HTTP date formats without locale or time-zone state.
     * @param text Entire field value, without surrounding optional whitespace.
     * @param now Reference instant for interpreting RFC 850 two-digit years more than 50 years ahead.
     * @return UTC seconds, or no value for invalid syntax, calendar dates, or clock fields.
     * @note A 64-bit seconds result avoids overflowing narrow system-clock calendar ranges.
     */
    [[nodiscard]] auto ParseHttpDate(
        std::string_view text,
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
    ) -> std::optional<std::chrono::sys_seconds> {
        using namespace std::chrono;
        constexpr std::array<std::string_view, 7> WEEKDAYS{ "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        constexpr std::array<std::string_view, 7> LONG_WEEKDAYS{
            "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
        };
        constexpr std::array<std::string_view, 12> MONTHS{
            "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
        };
        auto decimal = [](std::string_view value) -> std::optional<unsigned> {
            unsigned result{};
            for (auto byte : value) {
                if (byte < '0' || byte > '9') { return std::nullopt; }
                result = result * 10 + static_cast<unsigned>(byte - '0');
            }
            return value.empty() ? std::nullopt : std::optional{ result };
        };
        std::string_view weekday_name, month_name, day_text, year_text, clock;
        bool obsolete_year{};
        if (text.size() == 29 && text[3] == ',' && text[4] == ' ' && text[7] == ' '
            && text[11] == ' ' && text[16] == ' ' && text.substr(25) == " GMT") {
            weekday_name = text.substr(0, 3);
            day_text = text.substr(5, 2);
            month_name = text.substr(8, 3);
            year_text = text.substr(12, 4);
            clock = text.substr(17, 8);
        } else if (text.size() == 24 && text[3] == ' ' && text[7] == ' ' && text[10] == ' ' && text[19] == ' ') {
            weekday_name = text.substr(0, 3);
            month_name = text.substr(4, 3);
            day_text = text.substr(8, 2);
            if (day_text.front() == ' ') { day_text.remove_prefix(1); }
            clock = text.substr(11, 8);
            year_text = text.substr(20, 4);
        } else {
            auto const comma = text.find(',');
            if (comma == text.npos || text.size() - comma != 24) { return std::nullopt; }
            auto tail = text.substr(comma + 1);
            if (tail[0] != ' ' || tail[3] != '-' || tail[7] != '-' || tail[10] != ' '
                || tail.substr(19) != " GMT") { return std::nullopt; }
            weekday_name = text.substr(0, comma);
            day_text = tail.substr(1, 2);
            month_name = tail.substr(4, 3);
            year_text = tail.substr(8, 2);
            clock = tail.substr(11, 8);
            obsolete_year = true;
        }
        auto const& weekdays = obsolete_year ? LONG_WEEKDAYS : WEEKDAYS;
        auto const week = std::ranges::find(weekdays, weekday_name);
        auto const month_it = std::ranges::find(MONTHS, month_name);
        if (week == weekdays.end() || month_it == MONTHS.end() || clock[2] != ':' || clock[5] != ':') {
            return std::nullopt;
        }
        auto day_value = decimal(day_text), year_value = decimal(year_text);
        auto hour = decimal(clock.substr(0, 2)), minute = decimal(clock.substr(3, 2)), second = decimal(clock.substr(6, 2));
        if (!day_value || !year_value || !hour || !minute || !second || *hour > 23 || *minute > 59 || *second > 59) {
            return std::nullopt;
        }
        int calendar_year = static_cast<int>(*year_value);
        if (obsolete_year) {
            auto const current = static_cast<int>(year_month_day{ floor<days>(now) }.year());
            calendar_year += current / 100 * 100;
            if (calendar_year > current + 50) { calendar_year -= 100; }
        }
        year_month_day const date{ year{ calendar_year }, month{ static_cast<unsigned>(month_it - MONTHS.begin() + 1) },
            day{ *day_value } };
        if (!date.ok()) { return std::nullopt; }
        auto const calendar_day = sys_days{ date };
        if (weekday{ calendar_day }.c_encoding() != static_cast<unsigned>(week - weekdays.begin())) { return std::nullopt; }
        return sys_seconds{ calendar_day } + hours{ *hour } + minutes{ *minute } + seconds{ *second };
    }

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
