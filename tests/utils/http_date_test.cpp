/**
 * @file http_date_test.cpp
 * @brief Verify UTC HTTP date formatting at fixed instants and calendar boundaries.
 */
import std;
import mhttp;

#include "../support/test_support.hpp"

using mhttp::test::CheckEq;
using mhttp::test::Check;
using namespace std::chrono;

/**
 * @brief Verify a known HTTP timestamp, the epoch, leap day, and second flooring.
 */
auto fixed_http_dates() -> void {
    CheckEq(mhttp::FormatHttpDate(sys_days{ 1994y / November / 6 } + 8h + 49min + 37s),
        "Sun, 06 Nov 1994 08:49:37 GMT", "standard HTTP date example");
    CheckEq(mhttp::FormatHttpDate(system_clock::time_point{}),
        "Thu, 01 Jan 1970 00:00:00 GMT", "Unix epoch");
    CheckEq(mhttp::FormatHttpDate(sys_days{ 2024y / February / 29 } + 23h + 59min + 59s + 999ms),
        "Thu, 29 Feb 2024 23:59:59 GMT", "leap day and subsecond truncation");
    CheckEq(mhttp::FormatHttpDate(system_clock::time_point{} - 1ms),
        "Wed, 31 Dec 1969 23:59:59 GMT", "negative timestamps floor toward the previous second");
    CheckEq(mhttp::FormatHttpDate(sys_days{ 2000y / January / 1 }),
        "Sat, 01 Jan 2000 00:00:00 GMT", "century boundary");
}

/**
 * @brief Exercise every month and weekday without relying on localized calendar conversion.
 */
auto calendar_names() -> void {
    constexpr std::array<std::string_view, 12> MONTHS{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (unsigned month_number{ 1 }; month_number <= 12; ++month_number) {
        auto const formatted = mhttp::FormatHttpDate(sys_days{ 2026y / month{ month_number } / 1 });
        CheckEq(std::string_view{ formatted }.substr(8, 3), MONTHS[month_number - 1], "English month");
        CheckEq(formatted.size(), std::size_t{ 29 }, "IMF-fixdate has fixed width");
    }
    constexpr std::array<std::string_view, 7> WEEKDAYS{ "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    for (unsigned index{}; index < 7; ++index) {
        auto const formatted = mhttp::FormatHttpDate(sys_days{ 2026y / October / 18 } + days{ index });
        CheckEq(std::string_view{ formatted }.substr(0, 3), WEEKDAYS[index], "English weekday");
    }
}

/**
 * @brief Check independent date conversion calls from several threads.
 */
auto concurrent_http_dates() -> void {
    std::vector<std::jthread> workers;
    for (unsigned index{}; index < 4; ++index) {
        workers.emplace_back([] {
            for (unsigned repetition{}; repetition < 100; ++repetition) {
                CheckEq(mhttp::FormatHttpDate(sys_days{ 2026y / October / 21 } + 6h),
                    "Wed, 21 Oct 2026 06:00:00 GMT", "concurrent UTC conversion");
            }
        });
    }
}

/**
 * @brief Accept all HTTP date formats and reject partial, localized, and invalid calendar input.
 */
auto parsed_http_dates() -> void {
    auto const reference = sys_days{ 2026y / January / 1 };
    auto const expected = sys_days{ 1994y / November / 6 } + 8h + 49min + 37s;
    for (auto text : { "Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT",
        "Sun Nov  6 08:49:37 1994", "Sun Nov 06 08:49:37 1994" }) {
        CheckEq(mhttp::ParseHttpDate(text, reference), std::optional<sys_seconds>{ expected }, "three HTTP date formats agree");
    }
    for (auto text : { "", "Sun, 06 Nov 1994 08:49:37 UTC", "Sun, 06 Nov 1994 08:49:37 GMT extra",
        "Mon, 06 Nov 1994 08:49:37 GMT", "Sun, 31 Feb 1994 08:49:37 GMT", "Sun, 06 Nov 1994 24:00:00 GMT",
        "Sun, 06 Nov 1994 08:60:00 GMT", "Sun, 06 Nov 1994 08:49:99 GMT", "Sunday, 06-Nov-94 08:49:37",
        "Sun, 06 Nov 1994 08-49:37 GMT", "Sun Nov  0 08:49:37 1994" }) {
        Check(!mhttp::ParseHttpDate(text, reference), "malformed date is not a cache validator");
    }
    for (auto day : { sys_days{ 1969y / December / 31 }, sys_days{ 2000y / February / 29 }, sys_days{ 2100y / March / 1 } }) {
        CheckEq(mhttp::ParseHttpDate(mhttp::FormatHttpDate(day)), std::optional<sys_seconds>{ day }, "calendar date round trip");
    }
}

/**
 * @brief Run HTTP date cases.
 * @param argc Argument count.
 * @param argv Case filters.
 * @return Test runner exit status.
 */
auto main(int argc, char* argv[]) -> int {
    return mhttp::test::RunTests(argc, argv, {
        { "fixed HTTP dates", fixed_http_dates },
        { "calendar names", calendar_names },
        { "concurrent HTTP dates", concurrent_http_dates },
        { "parse HTTP dates", parsed_http_dates },
    });
}
