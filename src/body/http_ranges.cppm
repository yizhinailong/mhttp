/**
 * @file http_ranges.cppm
 * @brief Parse HTTP Range headers into inclusive byte ranges.
 */
export module mhttp:http_ranges;

import std;
import :strings;

namespace mhttp::detail {

    /**
     * @brief Byte ranges requested by a Range header (port of httplib::html::http_ranges).
     */
    class HttpRanges {
    public:
        using RangeType = std::pair<std::int64_t, std::int64_t>; ///< Inclusive [first, last].

        static constexpr std::size_t MAX_RANGES_DEFAULT{ 100 };

    private:
        std::vector<RangeType> m_ranges;               ///< Parsed ranges.
        std::size_t m_max_ranges{ MAX_RANGES_DEFAULT }; ///< Ranges accepted before rejecting.

    public:
        /**
         * @brief Parse a Range header for a resource of the given size.
         * @param range_str Header value; empty means "whole resource" and succeeds.
         * @param file_size Resource size in bytes.
         * @return False when the header is malformed or unsatisfiable (answer 416).
         */
        auto Parse(std::string_view range_str, std::size_t file_size) -> bool {
            m_ranges.clear();
            auto const size{ static_cast<std::int64_t>(file_size) };
            range_str = trim(range_str);
            if (range_str.empty()) {
                return true;
            }
            if (!range_str.starts_with("bytes=")) {
                return false;
            }
            range_str.remove_prefix(6);
            if (range_str.find("--") != std::string_view::npos) {
                return false;
            }
            if (range_str == "-") {
                m_ranges.emplace_back(0, size - 1);
                return true;
            }

            for (auto const range : split(range_str, ",")) {
                if (m_ranges.size() >= m_max_ranges) {
                    return fail();
                }
                if (range.starts_with("-")) {
                    // Suffix range "-N": the last N bytes.
                    std::int64_t count{ 0 };
                    auto const digits{ range.substr(1) };
                    auto const [end, ec]{ std::from_chars(digits.data(), digits.data() + digits.size(), count) };
                    if (ec != std::errc{} || end != digits.data() + digits.size() || count <= 0) {
                        return fail();
                    }
                    m_ranges.emplace_back(std::max<std::int64_t>(size - count, 0), size - 1);
                    continue;
                }

                // An empty or over-split part ("bytes=,", "bytes=5-10-20") is malformed.
                auto const bounds{ split(range, "-") };
                if (bounds.empty() || bounds.size() > 2) {
                    return fail();
                }
                std::int64_t first{ 0 };
                if (!parseNumber(trim(bounds[0]), first)) {
                    return fail();
                }
                std::int64_t last{ size - 1 };
                if (bounds.size() == 2 && !trim(bounds[1]).empty() && !parseNumber(trim(bounds[1]), last)) {
                    return fail();
                }
                if (first >= size || first > last) {
                    return fail();
                }
                m_ranges.emplace_back(first, std::min(last, size - 1));
            }
            return true;
        }

        [[nodiscard]] auto Size() const noexcept -> std::size_t {
            return m_ranges.size();
        }

        [[nodiscard]] auto Empty() const noexcept -> bool {
            return m_ranges.empty();
        }

        [[nodiscard]] auto Front() const -> RangeType const& {
            return m_ranges.front();
        }

        [[nodiscard]] auto At(std::size_t index) const -> RangeType const& {
            return m_ranges.at(index);
        }

        [[nodiscard]] auto Ranges() const noexcept -> std::vector<RangeType> const& {
            return m_ranges;
        }

        auto SetMaxRanges(std::size_t count) noexcept -> void {
            m_max_ranges = count;
        }

    private:
        /**
         * @brief Clear the result and report failure.
         * @return False.
         */
        auto fail() -> bool {
            m_ranges.clear();
            return false;
        }

        /**
         * @brief Parse a whole non-negative number.
         * @param text Digits.
         * @param out Receives the number.
         * @return True on success.
         */
        static auto parseNumber(std::string_view text, std::int64_t& out) -> bool {
            auto const [end, ec]{ std::from_chars(text.data(), text.data() + text.size(), out) };
            return !text.empty() && ec == std::errc{} && end == text.data() + text.size() && out >= 0;
        }
    };

} // namespace mhttp::detail
