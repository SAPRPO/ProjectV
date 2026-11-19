#include "tcp/logger.hpp"
#include "tcp/net_utils.hpp"
#include "tcp/protocol.hpp"
#include "tcp/socket_raii.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <netdb.h>  //added 


namespace {

using namespace std::chrono_literals;

struct Config {
    std::string host{"127.0.0.1"};
    std::uint16_t port{5555};
    std::string message;
    bool single_shot{false};  // false -> интерактивный режим
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds io_timeout{10000};
    tcp::LogLevel log_level{tcp::LogLevel::Info};
    std::string log_file;
};

void print_usage(const char* program) {
    std::cout
        << "Использование: " << program << " [host] [port] [message] [опции]\n"
        << "Опции:\n"
        << "  -H, --host <адрес>        адрес сервера (по умолчанию 127.0.0.1)\n"
        << "  -p, --port <порт>         порт сервера (по умолчанию 5555)\n"
        << "  -m, --message <текст>     отправить одно сообщение и выйти\n"
        << "  -i, --interactive         интерактивный режим (по умолчанию, если нет -m)\n"
        << "  -t, --timeout <сек>       таймаут чтения/записи (по умолчанию 10)\n"
        << "      --connect-timeout <мс> таймаут подключения (по умолчанию 5000)\n"
        << "      --log-level <уровень> trace|debug|info|warn|error|off\n"
        << "      --log-file <путь>     дублировать лог в файл\n"
        << "  -h, --help                показать эту справку\n";
}

bool parse_port(const std::string& text, std::uint16_t& out) {
    if (text.empty()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long value = std::stoul(text, &consumed, 10);
        if (consumed != text.size() || value == 0 || value > 65535UL) {
            return false;
        }
        out = static_cast<std::uint16_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_milliseconds(const std::string& text, std::chrono::milliseconds& out) {
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || value == 0) {
            return false;
        }
        out = std::chrono::milliseconds(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Config& config) {
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Опция " << flag << " требует значения\n";
                return {};
            }
            return argv[++i];
        };

        if (!arg.empty() && arg.front() != '-') {
            if (positional == 0) {
                config.host = arg;
            } else if (positional == 1) {
                if (!parse_port(arg, config.port)) {
                    std::cerr << "Некорректный порт: " << arg << "\n";
                    return false;
                }
            } else if (positional == 2) {
                config.message = arg;
                config.single_shot = true;
            } else {
                std::cerr << "Лишний аргумент: " << arg << "\n";
                return false;
            }
            ++positional;
            continue;
        }

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "-H" || arg == "--host") {
            config.host = value(arg.c_str());
            if (config.host.empty()) return false;
        } else if (arg == "-p" || arg == "--port") {
            const std::string text = value(arg.c_str());
            if (!parse_port(text, config.port)) {
                std::cerr << "Некорректный порт: " << text << "\n";
                return false;
            }
        } else if (arg == "-m" || arg == "--message") {
            config.message = value(arg.c_str());
            config.single_shot = true;
        } else if (arg == "-i" || arg == "--interactive") {
            config.single_shot = false;
        } else if (arg == "-t" || arg == "--timeout") {
            const std::string text = value(arg.c_str());
            std::chrono::milliseconds timeout{};
            if (!parse_milliseconds(text, timeout)) {
                std::cerr << "Некорректный таймаут: " << text << "\n";
                return false;
            }
            config.io_timeout = timeout * 1000;  // секунды -> миллисекунды
        } else if (arg == "--connect-timeout") {
            const std::string text = value(arg.c_str());
            if (!parse_milliseconds(text, config.connect_timeout)) {
                std::cerr << "Некорректный таймаут подключения: " << text << "\n";
                return false;
            }
        } else if (arg == "--log-level") {
            const std::string text = value(arg.c_str());
            if (!tcp::log_level_from_string(text, config.log_level)) {
                std::cerr << "Неизвестный уровень логирования: " << text << "\n";
                return false;
            }
        } else if (arg == "--log-file") {
            config.log_file = value(arg.c_str());
            if (config.log_file.empty()) return false;
        } else {
            std::cerr << "Неизвестная опция: " << arg << "\n";
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

/// Подключение к серверу с разрешением имени и таймаутом.
bool connect_to_server(const Config& config, tcp::Socket& socket, std::string* error) {
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

    struct AddrInfoGuard {
        addrinfo* ptr;
        ~AddrInfoGuard() {
            if (ptr != nullptr) {
                ::freeaddrinfo(ptr);
            }
        }
    } guard{result};

    for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
        tcp::Socket candidate(::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol));
        if (!candidate.valid()) {
            *error = "socket(): " + tcp::net::last_socket_error();
            continue;
        }

        const auto status = tcp::net::connect_with_timeout(
            candidate.get(), entry->ai_addr, static_cast<int>(entry->ai_addrlen),
            config.connect_timeout, error);
        if (status == tcp::net::IoStatus::Ok) {
            socket = std::move(candidate);
            return true;
        }
    }
    return false;
}

/// Отправляет запрос, печатает ответ. false — соединение/обмен не удались.
bool exchange(tcp::Socket& socket, const std::string& request, int& exit_code) {
    std::string error;

    LOG_DEBUG("отправляем запрос (", request.size(), " байт)");
    const auto sent = tcp::protocol::send_frame(socket.get(), request, &error);
    if (sent != tcp::net::IoStatus::Ok) {
        LOG_ERROR("не удалось отправить запрос: ", error);
        std::cerr << "Ошибка отправки: " << error << "\n";
        exit_code = 1;
        return false;
    }

    std::string response;
    const auto received = tcp::protocol::recv_frame(socket.get(), response, &error);
    if (received != tcp::net::IoStatus::Ok) {
        const std::string reason = error.empty() ? "соединение закрыто сервером" : error;
        LOG_ERROR("не удалось получить ответ: ", reason);
        std::cerr << "Ошибка получения ответа: " << reason << "\n";
        exit_code = 1;
        return false;
    }

    LOG_DEBUG("получен ответ (", response.size(), " байт)");
    std::cout << "Ответ сервера: " << response << std::endl;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    tcp::WinsockGuard winsock;
    if (!winsock.ok()) {
        std::cerr << "Не удалось инициализировать сетевую библиотеку\n";
        return 1;
    }

    Config config;
    if (!parse_args(argc, argv, config)) {
        return 2;
    }

    tcp::Logger::instance().set_level(config.log_level);
    if (!config.log_file.empty() && !tcp::Logger::instance().set_log_file(config.log_file)) {
        std::cerr << "Не удалось открыть файл лога: " << config.log_file << "\n";
    }

#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    tcp::Socket socket;
    std::string error;

    LOG_INFO("подключение к ", config.host, ":", config.port);
    if (!connect_to_server(config, socket, &error)) {
        LOG_ERROR("не удалось подключиться: ", error);
        std::cerr << "Не удалось подключиться к " << config.host << ":" << config.port << " — "
                  << error << "\n";
        return 1;
    }
    LOG_INFO("соединение установлено с ", config.host, ":", config.port);

    tcp::net::set_tcp_no_delay(socket.get());
    tcp::net::set_recv_timeout(socket.get(), config.io_timeout);
    tcp::net::set_send_timeout(socket.get(), config.io_timeout);

    int exit_code = 0;

    if (config.single_shot) {
        exchange(socket, config.message, exit_code);
    } else {
        std::cout << "Введите сообщение и нажмите Enter.\n"
                     "Пустая строка — пропустить, 'quit' или 'exit' — завершить.\n";
        std::string line;
        while (true) {
            std::cout << "> " << std::flush;
            if (!std::getline(std::cin, line)) {
                break;  // EOF
            }
            if (tcp::protocol::is_quit_command(line)) {
                break;
            }
            if (line.empty()) {
                continue;
            }
            if (!exchange(socket, line, exit_code)) {
                break;
            }
        }
    }

    tcp::net::shutdown_socket(socket.get());
    LOG_INFO("клиент завершил работу");
    return exit_code;
}
