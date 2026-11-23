

#include "tcp/logger.hpp"
#include "tcp/net_utils.hpp"
#include "tcp/protocol.hpp"
#include "tcp/socket_raii.hpp"

#ifdef _WIN32
#  include <ws2tcpip.h>   // getaddrinfo, gai_strerror
#else
#  include <netdb.h>      // getaddrinfo, gai_strerror, freeaddrinfo, addrinfo
#endif


#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

enum ExitCode { kOk = 0, kConnectFailed = 1, kBadUsage = 2, kIoTimeout = 3,
                kServerClosed = 4, kServerError = 5 };

struct Config {
    std::string  host{"127.0.0.1"};
    std::uint16_t port{5555};
    std::string  message;
    bool single_shot{false};
    int retries{3};
    std::chrono::milliseconds retry_delay{500};
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds io_timeout{10000};
    tcp::LogLevel log_level{tcp::LogLevel::Info};
    std::string  log_file;
};

void print_usage(const char* program) {
    std::cout << "Использование: " << program << " [host] [port] [message] [опции]\n"
              << "  -H, --host <адрес>            хост (127.0.0.1)\n"
              << "  -p, --port <порт>             порт (5555)\n"
              << "  -m, --message <текст>         одно сообщение и выход\n"
              << "  -i, --interactive             интерактивно\n"
              << "      --retries <N>             попыток подключения (3)\n"
              << "      --retry-delay <мс>        базовая задержка (500, ×2, cap 5с)\n"
              << "      --connect-timeout <мс>    (5000)\n"
              << "  -t, --timeout <сек>           тайм-аут обмена (10)\n"
              << "      --log-level <уровень>     trace|debug|info|warn|error|off\n"
              << "      --log-file <путь>\n"
              << "  -h, --help\n";
}

template <typename T>
bool parse_uint(const std::string& text, T& out, T low, T high) {
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || value < low || value > high) return false;
        out = static_cast<T>(value);
        return true;
    } catch (const std::exception&) { return false; }
}

bool parse_args(int argc, char** argv, Config& config) {
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::cerr << "Опция " << flag << " требует значения\n"; return {}; }
            return argv[++i];
        };
        if (!arg.empty() && arg.front() != '-') {
            if (positional == 0) config.host = arg;
            else if (positional == 1) {
                std::uint32_t p = 0;
                if (!parse_uint(arg, p, 1u, 65535u)) { std::cerr << "Плохой порт: " << arg << "\n"; return false; }
                config.port = static_cast<std::uint16_t>(p);
            }
            else if (positional == 2) { config.message = arg; config.single_shot = true; }
            else { std::cerr << "Лишний аргумент: " << arg << "\n"; return false; }
            ++positional;
            continue;
        }
        if (arg == "-h" || arg == "--help") { print_usage(argv[0]); std::exit(0); }
        else if (arg == "-H" || arg == "--host") { config.host = value(arg.c_str()); if (config.host.empty()) return false; }
        else if (arg == "-p" || arg == "--port") {
            std::uint32_t p = 0;
            if (!parse_uint(value(arg.c_str()), p, 1u, 65535u)) return false;
            config.port = static_cast<std::uint16_t>(p);
        }
        else if (arg == "-m" || arg == "--message") { config.message = value(arg.c_str()); config.single_shot = true; }
        else if (arg == "-i" || arg == "--interactive") { config.single_shot = false; }
        else if (arg == "--retries") {
            std::uint32_t n = 0;
            if (!parse_uint(value(arg.c_str()), n, 0u, 100u)) return false;
            config.retries = static_cast<int>(n);
        }
        else if (arg == "--retry-delay") {
            std::uint32_t ms = 0;
            if (!parse_uint(value(arg.c_str()), ms, 0u, 60000u)) return false;
            config.retry_delay = std::chrono::milliseconds(ms);
        }
        else if (arg == "--connect-timeout") {
            std::uint32_t ms = 0;
            if (!parse_uint(value(arg.c_str()), ms, 1u, 600000u)) return false;
            config.connect_timeout = std::chrono::milliseconds(ms);
        }
        else if (arg == "-t" || arg == "--timeout") {
            std::uint32_t s = 0;
            if (!parse_uint(value(arg.c_str()), s, 1u, 86400u)) return false;
            config.io_timeout = std::chrono::seconds(s);
        }
        else if (arg == "--log-level") {
            if (!tcp::log_level_from_string(value(arg.c_str()), config.log_level)) return false;
        }
        else if (arg == "--log-file") { config.log_file = value(arg.c_str()); if (config.log_file.empty()) return false; }
        else { std::cerr << "Неизвестная опция: " << arg << "\n"; return false; }
    }
    return true;
}

bool connect_once(const Config& config, tcp::Socket& socket, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port_text = std::to_string(config.port);
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(config.host.c_str(), port_text.c_str(), &hints, &result);
    if (rc != 0) {
#ifdef _WIN32
        const std::string reason = ::gai_strerrorA(rc);
#else
        const std::string reason = ::gai_strerror(rc);
#endif
        *error = "getaddrinfo(" + config.host + "): " + reason;
        return false;
    }
    struct AddrGuard { addrinfo* p; ~AddrGuard() { if (p) ::freeaddrinfo(p); } } guard{result};

    for (addrinfo* entry = result; entry; entry = entry->ai_next) {
        tcp::Socket candidate(::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol));
        if (!candidate.valid()) { *error = "socket(): " + tcp::net::last_socket_error(); continue; }
        const auto status = tcp::net::connect_with_timeout(
            candidate.get(), entry->ai_addr, static_cast<int>(entry->ai_addrlen),
            config.connect_timeout, error);
        if (status == tcp::net::IoStatus::Ok) { socket = std::move(candidate); return true; }
    }
    return false;
}

bool connect_with_retry(const Config& config, tcp::Socket& socket, std::string* error) {
    auto delay = config.retry_delay;
    for (int attempt = 0; attempt <= config.retries; ++attempt) {
        LOG_DEBUG("попытка подключения ", attempt + 1, "/", config.retries + 1);
        if (connect_once(config, socket, error)) return true;
        if (attempt == config.retries) break;
        LOG_WARN("подключение не удалось (", *error, "), повтор через ", delay.count(), " ms");
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds(5000));
    }
    return false;
}

/// Одна пара запрос/ответ. false — соединение непригодно дальше.
bool exchange(tcp::Socket& socket, const std::string& request, const Config& config, int& exit_code) {
    std::string error;

    const auto send_deadline = tcp::net::deadline_from_now(config.io_timeout);
    auto status = tcp::protocol::send_frame_until(socket.get(), request, send_deadline, &error);
    if (status != tcp::net::IoStatus::Ok) {
        std::cerr << "Не удалось отправить запрос: " << error << "\n";
        exit_code = (status == tcp::net::IoStatus::Timeout) ? kIoTimeout : kServerClosed;
        return false;
    }

    const auto recv_deadline = tcp::net::deadline_from_now(config.io_timeout);
    std::string response;
    status = tcp::protocol::recv_frame_until(socket.get(), response, recv_deadline, &error);

    if (status == tcp::net::IoStatus::Ok) {
        std::cout << "Ответ сервера: " << response << "\n";
        if (response.starts_with("ERROR ")) exit_code = kServerError;
        return true;
    }
    if (status == tcp::net::IoStatus::Closed) {
        std::cerr << "Сервер закрыл соединение\n";
        exit_code = kServerClosed;
        return false;
    }
    if (status == tcp::net::IoStatus::Timeout) {
        std::cerr << "Тайм-аут обмена: " << error << "\n";
        exit_code = kIoTimeout;
        return false;
    }
    std::cerr << "Ошибка обмена: " << error << "\n";
    exit_code = kConnectFailed;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    tcp::WinsockGuard winsock;
    if (!winsock.ok()) { std::cerr << "Winsock init failed\n"; return 1; }

    Config config;
    if (!parse_args(argc, argv, config)) return kBadUsage;

    tcp::Logger::instance().set_level(config.log_level);
    if (!config.log_file.empty() && !tcp::Logger::instance().set_log_file(config.log_file))
        std::cerr << "Не удалось открыть файл лога: " << config.log_file << "\n";

#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    tcp::Socket socket;
    std::string error;
    if (!connect_with_retry(config, socket, &error)) {
        std::cerr << "Не удалось подключиться к " << config.host << ":" << config.port
                  << " — " << error << "\n";
        return kConnectFailed;
    }
    LOG_INFO("соединение установлено с ", config.host, ":", config.port);

    tcp::net::set_tcp_no_delay(socket.get());

    int exit_code = kOk;
    if (config.single_shot) {
        exchange(socket, config.message, config, exit_code);
    } else {
        std::cout << "Введите сообщение и Enter. 'quit'/'exit' — выход.\n";
        std::string line;
        while (true) {
            std::cout << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (tcp::protocol::is_quit_command(line)) break;
            if (line.empty()) continue;
            if (!exchange(socket, line, config, exit_code)) break;
        }
    }

    tcp::net::shutdown_socket(socket.get());
    LOG_INFO("клиент завершил работу (exit=", exit_code, ")");
    return exit_code;
}
