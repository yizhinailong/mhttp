/**
 * @file logging.cppm
 * @brief Provide the shared console sink and a replaceable per-component logger.
 */
module;

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

export module mhttp:logging;

import std;

namespace mhttp::detail {

    /**
     * @brief Return the process-wide colored stdout sink.
     * @return Sink shared by every default logger so console writes never interleave.
     */
    auto console_sink() -> std::shared_ptr<spdlog::sinks::sink> {
        static auto s_sink{ std::make_shared<spdlog::sinks::stdout_color_sink_mt>() };
        return s_sink;
    }

    /**
     * @brief Create an info-level logger on the shared console sink.
     * @param name Logger name shown in each record.
     * @return Newly created logger.
     */
    auto make_console_logger(std::string_view name) -> std::shared_ptr<spdlog::logger> {
        auto logger{ std::make_shared<spdlog::logger>(std::string{ name }, console_sink()) };
        logger->set_level(spdlog::level::info);
        return logger;
    }

    /**
     * @brief Hold a default logger that callers may replace at any time.
     */
    class LoggerHolder {
    private:
        std::shared_ptr<spdlog::logger> m_default_logger;             ///< Fallback console logger.
        std::atomic<std::shared_ptr<spdlog::logger>> m_custom_logger; ///< Caller-provided override.

    public:
        /**
         * @brief Create the default console logger.
         * @param name Logger name shown in each record.
         */
        explicit LoggerHolder(std::string_view name)
            : m_default_logger{ make_console_logger(name) } {}

        /**
         * @brief Return the active logger.
         * @return Custom logger when set, otherwise the default console logger.
         * @note Thread-safe.
         */
        [[nodiscard]] auto GetLogger() const -> std::shared_ptr<spdlog::logger> {
            auto logger{ m_custom_logger.load() };
            return logger ? logger : m_default_logger;
        }

        /**
         * @brief Replace the active logger.
         * @param logger New logger; nullptr restores the default console logger.
         * @note Thread-safe; takes effect for subsequent records.
         */
        auto SetLogger(std::shared_ptr<spdlog::logger> logger) -> void {
            m_custom_logger.store(std::move(logger));
        }
    };

} // namespace mhttp::detail
