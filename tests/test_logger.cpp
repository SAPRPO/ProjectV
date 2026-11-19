#include "test_framework.hpp"
#include "tcp/logger.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path temp_log_path(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

/// Восстанавливает настройки логгера после теста.
struct LoggerScope {
    ~LoggerScope() {
        tcp::Logger::instance().set_log_file("");
        tcp::Logger::instance().set_level(tcp::LogLevel::Info);
    }
};

}  // namespace

TCP_TEST_CASE(level_names) {
    TCP_CHECK_EQ(std::string(tcp::to_string(tcp::LogLevel::Trace)), std::string("TRACE"));
    TCP_CHECK_EQ(std::string(tcp::to_string(tcp::LogLevel::Info)), std::string("INFO"));
    TCP_CHECK_EQ(std::string(tcp::to_string(tcp::LogLevel::Error)), std::string("ERROR"));
    TCP_CHECK_EQ(std::string(tcp::to_string(tcp::LogLevel::Off)), std::string("OFF"));
}

TCP_TEST_CASE(level_parsing) {
    tcp::LogLevel level{};

    TCP_CHECK(tcp::log_level_from_string("debug", level));
    TCP_CHECK(level == tcp::LogLevel::Debug);

    TCP_CHECK(tcp::log_level_from_string("WARNING", level));
    TCP_CHECK(level == tcp::LogLevel::Warn);

    TCP_CHECK(tcp::log_level_from_string("Off", level));
    TCP_CHECK(level == tcp::LogLevel::Off);

    TCP_CHECK(!tcp::log_level_from_string("nonsense", level));
    TCP_CHECK(!tcp::log_level_from_string("", level));
}

TCP_TEST_CASE(format_message_concatenates) {
    TCP_CHECK_EQ(tcp::format_message("a", 1, 'b', 2.5), std::string("a1b2.5"));
    TCP_CHECK_EQ(tcp::format_message("x = ", 42), std::string("x = 42"));
}

TCP_TEST_CASE(level_filtering_with_file_output) {
    LoggerScope scope;
    const auto path = temp_log_path("tcp_project_logger_test.log");
    std::filesystem::remove(path);

    tcp::Logger& logger = tcp::Logger::instance();
    TCP_REQUIRE(logger.set_log_file(path.string()));
    logger.set_level(tcp::LogLevel::Warn);

    logger.log(tcp::LogLevel::Debug, "отладочное сообщение");
    logger.log(tcp::LogLevel::Info, "информационное сообщение");
    logger.log(tcp::LogLevel::Error, "сообщение об ошибке");

    const std::string content = read_file(path);
    TCP_CHECK(content.find("сообщение об ошибке") != std::string::npos);
    TCP_CHECK(content.find("информационное сообщение") == std::string::npos);
    TCP_CHECK(content.find("отладочное сообщение") == std::string::npos);

    // Формат строки: время, уровень, идентификатор потока
    TCP_CHECK(content.find("[ERROR]") != std::string::npos);
    TCP_CHECK(content.find("tid=") != std::string::npos);
    TCP_CHECK(content.find("20") != std::string::npos);

    std::filesystem::remove(path);
}

TCP_TEST_CASE(macros_respect_level) {
    LoggerScope scope;
    const auto path = temp_log_path("tcp_project_logger_macros.log");
    std::filesystem::remove(path);

    tcp::Logger& logger = tcp::Logger::instance();
    TCP_REQUIRE(logger.set_log_file(path.string()));
    logger.set_level(tcp::LogLevel::Info);

    LOG_DEBUG("не должно попасть в файл");
    LOG_INFO("должно попасть: ", 42, " и ", "строка");

    const std::string content = read_file(path);
    TCP_CHECK(content.find("не должно попасть") == std::string::npos);
    TCP_CHECK(content.find("должно попасть: 42 и строка") != std::string::npos);

    std::filesystem::remove(path);
}

int main() {
    return testfw::run_all();
}
