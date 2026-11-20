#include "tcp/logger.hpp"
#include "tcp/net_utils.hpp"
#include "tcp/protocol.hpp"
#include "tcp/socket_raii.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};

}  // namespace

extern "C" void tcp_server_signal_handler(int) {
    // Асинхронно-безопасная операция: только сохранение флага.
    g_stop.store(true, std::memory_order_relaxed);
}

namespace {

struct Config {
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{5555};
    std::size_t max_connections{64};
    std::chrono::milliseconds io_timeout{30000};
    tcp::LogLevel log_level{tcp::LogLevel::Info};
    std::string log_file;
};

void print_usage(const char* program) {
    std::cout
        << "Использование: " << program << " [опции]\n"
        << "Опции:\n"
        << "  -b, --bind <адрес>        адрес для прослушивания (по умолчанию 0.0.0.0)\n"
        << "  -p, --port <порт>         TCP-порт (по умолчанию 5555)\n"
        << "      --max-connections <N> лимит одновременных подключений (по умолчанию 64)\n"
        << "      --io-timeout <сек>    таймаут чтения/записи (по умолчанию 30)\n"
        << "      --log-level <уровень> trace|debug|info|warn|error|off (по умолчанию info)\n"
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

bool parse_size(const std::string& text, std::size_t& out) {
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || value == 0) {
            return false;
        }
        out = static_cast<std::size_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_seconds(const std::string& text, std::chrono::milliseconds& out) {
    std::size_t seconds = 0;
    if (!parse_size(text, seconds)) {
        return false;
    }
    out = std::chrono::seconds(seconds);
    return true;
}

bool parse_args(int argc, char** argv, Config& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Опция " << flag << " требует значения\n";
                return {};
            }
            return argv[++i];
        };

        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "-b" || arg == "--bind") {
            config.bind_address = value(arg.c_str());
            if (config.bind_address.empty()) return false;
        } else if (arg == "-p" || arg == "--port") {
            const std::string text = value(arg.c_str());
            if (!parse_port(text, config.port)) {
                std::cerr << "Некорректный порт: " << text << "\n";
                return false;
            }
        } else if (arg == "--max-connections") {
            const std::string text = value(arg.c_str());
            if (!parse_size(text, config.max_connections)) {
                std::cerr << "Некорректный лимит подключений: " << text << "\n";
                return false;
            }
        } else if (arg == "--io-timeout") {
            const std::string text = value(arg.c_str());
            if (!parse_seconds(text, config.io_timeout)) {
                std::cerr << "Некорректный таймаут: " << text << "\n";
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

/// Сервер: слушает порт и обслуживает каждое соединение в отдельном потоке.
class Server {
public:
    explicit Server(Config config) : config_(std::move(config)) {}

    bool start(std::string* error);
    void run();

private:
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    /// Гарантирует снятие соединения с учёта и закрытие сокета даже при исключении.
    struct ConnectionGuard {
        Server* server{nullptr};
        tcp::Socket* socket{nullptr};

        ~ConnectionGuard() {
            if (server != nullptr && socket != nullptr) {
                server->release_connection(*socket);
            }
        }
    };

    void handle_client(tcp::socket_t client_fd, std::string peer);
    void reap_finished();
    void register_connection(tcp::socket_t socket_fd);
    void release_connection(tcp::Socket& socket);
    void shutdown_all_connections();

    Config config_;
    tcp::Socket listener_;

    std::mutex workers_mutex_;
    std::vector<Worker> workers_;

    std::atomic<std::size_t> active_connections_{0};

    // Реестр активных подключений. Дескриптор закрывается только под этим
    // мьютексом (см. release_connection), поэтому shutdown() никогда не попадёт
    // на уже переиспользованный дескриптор.
    std::mutex connections_mutex_;
    std::vector<tcp::socket_t> connections_;
};

bool Server::start(std::string* error) {
    listener_.reset(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener_.valid()) {
        if (error) *error = "socket(): " + tcp::net::last_socket_error();
        return false;
    }

    if (!tcp::net::set_reuse_address(listener_.get())) {
        LOG_WARN("не удалось установить SO_REUSEADDR: ", tcp::net::last_socket_error());
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        if (error) *error = "некорректный адрес: " + config_.bind_address;
        return false;
    }

    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
               static_cast<tcp::net::socklen_type>(sizeof(address))) != 0) {
        if (error) *error = "bind(" + config_.bind_address + ":" + std::to_string(config_.port) +
                            "): " + tcp::net::last_socket_error();
        return false;
    }

    if (::listen(listener_.get(), SOMAXCONN) != 0) {
        if (error) *error = "listen(): " + tcp::net::last_socket_error();
        return false;
    }

    // accept() вызывается только после poll(), но неблокирующий режим
    // защищает от редких гонок, когда соединение уже сброшено.
    if (!tcp::net::set_nonblocking(listener_.get(), true, error)) {
        return false;
    }
    return true;
}

void Server::run() {
    LOG_INFO("сервер запущен: ", config_.bind_address, ":", config_.port,
             " (макс. подключений: ", config_.max_connections, ")");

    std::string error;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto waited = tcp::net::wait_readable(listener_.get(), 200ms, &error);
        if (waited == tcp::net::WaitResult::Timeout || waited == tcp::net::WaitResult::Interrupted) {
            reap_finished();
            continue;
        }
        if (waited == tcp::net::WaitResult::Error) {
            LOG_ERROR("ошибка ожидания на слушающем сокете: ", error);
            break;
        }

        sockaddr_in peer{};
        auto peer_length = static_cast<tcp::net::socklen_type>(sizeof(peer));
        const tcp::socket_t client_fd =
            ::accept(listener_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_length);

        if (!tcp::socket_valid(client_fd)) {
            const int code = tcp::net::last_socket_error_code();
            if (tcp::net::is_interrupted_error(code) || tcp::net::is_would_block_error(code)) {
                continue;
            }
            LOG_WARN("accept(): ", tcp::net::last_socket_error());
            continue;
        }

        const std::string peer_name = tcp::net::describe_peer(peer);
        const std::size_t active = active_connections_.load(std::memory_order_relaxed);

        if (active >= config_.max_connections) {
            LOG_WARN("лимит подключений исчерпан (", active, "/", config_.max_connections,
                     "), отклонено соединение от ", peer_name);
            tcp::Socket rejected(client_fd);
            tcp::net::shutdown_socket(rejected.get());
            continue;  // ~Socket закроет дескриптор
        }

        active_connections_.fetch_add(1, std::memory_order_relaxed);

        auto finished = std::make_shared<std::atomic<bool>>(false);
        try {
            Worker worker;
            worker.finished = finished;
            worker.thread = std::thread([this, client_fd, peer_name, finished]() {
                try {
                    handle_client(client_fd, peer_name);
                } catch (const std::exception& ex) {
                    LOG_ERROR("необработанное исключение в соединении ", peer_name, ": ", ex.what());
                } catch (...) {
                    LOG_ERROR("необработанное исключение в соединении ", peer_name);
                }
                active_connections_.fetch_sub(1, std::memory_order_relaxed);
                finished->store(true, std::memory_order_release);
            });

            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers_.push_back(std::move(worker));
        } catch (const std::exception& ex) {
            LOG_ERROR("не удалось создать поток для ", peer_name, ": ", ex.what());
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            tcp::Socket failed(client_fd);  // дескриптор не попал в поток — закрываем здесь
        }

        reap_finished();
    }

    LOG_INFO("остановка сервера: прекращаем приём новых подключений");
    listener_.reset();
    shutdown_all_connections();

    std::vector<Worker> pending;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        pending.swap(workers_);
    }
    for (auto& worker : pending) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
    LOG_INFO("сервер остановлен");
}

void Server::handle_client(tcp::socket_t client_fd, std::string peer) {
    tcp::Socket socket(client_fd);
    const ConnectionGuard guard{this, &socket};

    register_connection(client_fd);
    LOG_INFO("подключился клиент: ", peer);

    if (!tcp::net::set_tcp_no_delay(socket.get())) {
        LOG_WARN("не удалось включить TCP_NODELAY для ", peer);
    }
    if (!tcp::net::set_recv_timeout(socket.get(), config_.io_timeout)) {
        LOG_WARN("не удалось задать таймаут чтения для ", peer);
    }
    if (!tcp::net::set_send_timeout(socket.get(), config_.io_timeout)) {
        LOG_WARN("не удалось задать таймаут записи для ", peer);
    }

    std::size_t request_count = 0;
    std::string error;

    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto waited = tcp::net::wait_readable(socket.get(), 250ms, nullptr);
        if (waited == tcp::net::WaitResult::Timeout || waited == tcp::net::WaitResult::Interrupted) {
            continue;  // проверяем флаг остановки и снова ждём
        }
        if (waited == tcp::net::WaitResult::Error) {
            LOG_WARN("ошибка ожидания данных от ", peer);
            break;
        }

        std::string request;
        const auto status = tcp::protocol::recv_frame(socket.get(), request, &error);

        if (status == tcp::net::IoStatus::Closed) {
            LOG_INFO("клиент отключился: ", peer);
            break;
        }
        if (status == tcp::net::IoStatus::Timeout) {
            LOG_WARN("таймаут чтения, соединение закрывается: ", peer);
            break;
        }
        if (status == tcp::net::IoStatus::Error) {
            LOG_ERROR("ошибка чтения (", peer, "): ", error);
            break;
        }

        ++request_count;
        LOG_DEBUG("запрос #", request_count, " от ", peer, " (", request.size(), " байт): \"",
                  tcp::protocol::preview(request), "\"");

        const std::string response = tcp::protocol::make_response(request);
        const auto sent = tcp::protocol::send_frame(socket.get(), response, &error);
        if (sent != tcp::net::IoStatus::Ok) {
            LOG_ERROR("ошибка отправки (", peer, "): ", error);
            break;
        }
    }

    tcp::net::shutdown_socket(socket.get());
    LOG_INFO("соединение завершено: ", peer, " (обработано запросов: ", request_count, ")");
}

void Server::register_connection(tcp::socket_t socket_fd) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.push_back(socket_fd);
}

void Server::release_connection(tcp::Socket& socket) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    const tcp::socket_t fd = socket.get();
    connections_.erase(std::remove(connections_.begin(), connections_.end(), fd),
                       connections_.end());
    socket.reset();  // закрываем дескриптор под мьютексом: исключаем гонку с shutdown
}

void Server::shutdown_all_connections() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (tcp::socket_t fd : connections_) {
        tcp::net::shutdown_socket(fd);
    }
    if (!connections_.empty()) {
        LOG_INFO("инициировано закрытие активных соединений: ", connections_.size());
    }
}

void Server::reap_finished() {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto it = workers_.begin(); it != workers_.end();) {
        if (it->finished != nullptr && it->finished->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = workers_.erase(it);
        } else {
            ++it;
        }
    }
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
    std::signal(SIGPIPE, SIG_IGN);  // разрыв соединения не должен убивать процесс
#endif
    std::signal(SIGINT, tcp_server_signal_handler);
    std::signal(SIGTERM, tcp_server_signal_handler);

    Server server(config);
    std::string error;
    if (!server.start(&error)) {
        LOG_ERROR("не удалось запустить сервер: ", error);
        return 1;
    }

    server.run();
    return 0;
}
