#include "tcp/logger.hpp"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <functional>
#include <thread>

namespace tcp {
namespace {

constexpr const char* kLevelNames[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "OFF"};

}  // namespace

std::string_view to_string(LogLevel level) noexcept {
    const int index = static_cast<int>(level);
    if (index < 0 || index > static_cast<int>(LogLevel::Off)) {
        return "UNKNOWN";
    }
    return kLevelNames[index];
}

bool log_level_from_string(std::string_view text, LogLevel& out) noexcept {
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    static const struct {
        const char* name;
        LogLevel level;
    } kMap[] = {
        {"trace", LogLevel::Trace}, {"debug", LogLevel::Debug}, {"info", LogLevel::Info},
        {"warn", LogLevel::Warn},   {"warning", LogLevel::Warn}, {"error", LogLevel::Error},
        {"off", LogLevel::Off},     {"none", LogLevel::Off},
    };

    for (const auto& entry : kMap) {
        if (lower == entry.name) {
            out = entry.level;
            return true;
        }
    }
    return false;
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::set_level(LogLevel level) noexcept {
    level_.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel Logger::level() const noexcept {
    return static_cast<LogLevel>(level_.load(std::memory_order_relaxed));
}

bool Logger::set_log_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_.close();
    if (path.empty()) {
        return true;  // дублирование отключено
    }
    file_.open(path, std::ios::out | std::ios::app);
    return file_.is_open();
}

std::string Logger::make_header(LogLevel level) {
    using namespace std::chrono;

    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const long long millis =
        static_cast<long long>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif

    char date[32] = {0};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &tm);

    const auto thread_hash =
        static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

    char header[160] = {0};
    std::snprintf(header, sizeof(header), "[%s.%03lld] [%-5s] [tid=%llx] ", date, millis,
                  std::string(to_string(level)).c_str(), thread_hash);
    return header;
}

void Logger::log(LogLevel level, std::string_view message) {
    if (static_cast<int>(level) < level_.load(std::memory_order_relaxed)) {
        return;  // отфильтровано по уровню
    }

    std::string line = make_header(level);
    line.append(message.data(), message.size());
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(mutex_);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);

    if (file_.is_open()) {
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.flush();
    }
}

}  // namespace tcp
