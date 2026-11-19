#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace tcp {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Off };

/// "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "OFF".
std::string_view to_string(LogLevel level) noexcept;

/// Разбор строки уровня ("info", "WARNING", "off", ...), регистр не важен.
bool log_level_from_string(std::string_view text, LogLevel& out) noexcept;

/// Потокобезопасный логгер (синглтон).
class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) noexcept;
    LogLevel level() const noexcept;

    /// Дублировать вывод в файл (пустая строка — отключить дублирование).
    bool set_log_file(const std::string& path);

    /// Основной метод записи. Обычно вызывается через макрос LOG_INFO(...).
    void log(LogLevel level, std::string_view message);

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static std::string make_header(LogLevel level);

    std::atomic<int> level_{static_cast<int>(LogLevel::Info)};
    std::mutex mutex_;
    std::ofstream file_;
};

/// Склеивает аргументы в строку (упрощённая замена std::format из C++20).
template <typename... Args>
std::string format_message(Args&&... args) {
    std::ostringstream out;
    (out << ... << std::forward<Args>(args));
    return out.str();
}

}  // namespace tcp

#define TCP_LOG(level, ...) \
    ::tcp::Logger::instance().log((level), ::tcp::format_message(__VA_ARGS__))

#define LOG_TRACE(...) TCP_LOG(::tcp::LogLevel::Trace, __VA_ARGS__)
#define LOG_DEBUG(...) TCP_LOG(::tcp::LogLevel::Debug, __VA_ARGS__)
#define LOG_INFO(...)  TCP_LOG(::tcp::LogLevel::Info,  __VA_ARGS__)
#define LOG_WARN(...)  TCP_LOG(::tcp::LogLevel::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) TCP_LOG(::tcp::LogLevel::Error, __VA_ARGS__)
