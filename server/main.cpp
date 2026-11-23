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

std::atomic<bool>   g_stop{false};
std::atomic<int>    g_signal_count{0};
std::atomic<std::uint64_t> g_conn_seq{0};

}  // namespace

extern "C" void tcp_server_signal_handler(int) {
    const int prev = g_signal_count.fetch_add(1, std::memory_order_relaxed);
    g_stop.store(true, std::memory_order_relaxed);
    if (prev >= 1) {
        // второй сигнал — не ждём, выходим немедленно
        ::_exit(130);
    }
}

namespace {

struct Config {
    std::string  bind_address{"0.0.0.0"};
    std::uint16_t port{5555};
    std::size_t  max_connections{64};
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds request_timeout{5000};
    std::size_t  max_request_size{64 * 1024};
    tcp::LogLevel log_level{tcp::LogLevel::Info};
    std::string  log_file;
};

void print_usage(const char* program) {
    std::cout << "Использование: " << program << " [опции]\n"
              << "  -b, --bind <адрес>          адрес (по умолчанию 0.0.0.0)\n"
              << "  -p, --port <порт>           порт (по умолчанию 5555)\n"
              << "      --max-connections <N>   лимит соединений (64)\n"
              << "      --idle-timeout <сек>    молчащий клиент (60)\n"
              << "      --request-timeout <сек> недочитанный кадр (5)\n"
              << "      --max-request-size <N>  максимум payload в байтах (65536)\n"
              << "      --log-level <уровень>   trace|debug|info|warn|error|off\n"
              << "      --log-file <путь>       дублировать лог в файл\n"
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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::cerr << "Опция " << flag << " требует значения\n"; return {}; }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") { print_usage(argv[0]); std::exit(0); }
        else if (arg == "-b" || arg == "--bind") { config.bind_address = value(arg.c_str()); if (config.bind_address.empty()) return false; }
        else if (arg == "-p" || arg == "--port") {
            std::uint32_t p = 0;
            if (!parse_uint(value(arg.c_str()), p, 1u, 65535u)) { std::cerr << "Плохой порт\n"; return false; }
            config.port = static_cast<std::uint16_t>(p);
        }
        else if (arg == "--max-connections") {
            if (!parse_uint(value(arg.c_str()), config.max_connections, std::size_t{1}, std::size_t{1} << 20)) return false;
        }
        else if (arg == "--idle-timeout") {
            std::size_t s = 0;
            if (!parse_uint(value(arg.c_str()), s, std::size_t{1}, std::size_t{86400})) return false;
            config.idle_timeout = std::chrono::seconds(s);
        }
        else if (arg == "--request-timeout") {
            std::size_t s = 0;
            if (!parse_uint(value(arg.c_str()), s, std::size_t{1}, std::size_t{86400})) return false;
            config.request_timeout = std::chrono::seconds(s);
        }
        else if (arg == "--max-request-size") {
            if (!parse_uint(value(arg.c_str()), config.max_request_size, std::size_t{1}, std::size_t{1} << 30)) return false;
        }
        else if (arg == "--log-level") {
            if (!tcp::log_level_from_string(value(arg.c_str()), config.log_level)) return false;
        }
        else if (arg == "--log-file") {
            config.log_file = value(arg.c_str());
            if (config.log_file.empty()) return false;
        }
        else { std::cerr << "Неизвестная опция: " << arg << "\n"; return false; }
    }
    return true;
}

class Server {
public:
    explicit Server(Config config) : config_(std::move(config)) {}

    bool start(std::string* error);
    void run();

private:
    struct Worker {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };
    struct ConnectionGuard {
        Server* server = nullptr;
        tcp::Socket* socket = nullptr;
        ~ConnectionGuard() {
            if (server && socket) server->release_connection(*socket);
        }
    };

    void handle_client(tcp::socket_t fd, std::string peer, std::uint64_t conn_id);
    void reap_finished();
    void register_connection(tcp::socket_t fd);
    void release_connection(tcp::Socket& socket);
    void shutdown_all_connections();

    Config config_;
    tcp::Socket listener_;

    std::mutex workers_mutex_;
    std::vector<Worker> workers_;

    std::atomic<std::size_t> active_connections_{0};
    std::atomic<std::uint64_t> total_connections_{0};
    std::atomic<std::uint64_t> total_requests_{0};
    std::atomic<std::uint64_t> total_errors_{0};

    std::mutex connections_mutex_;
    std::vector<tcp::socket_t> connections_;
};

bool Server::start(std::string* error) {
    listener_.reset(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener_.valid()) { if (error) *error = "socket(): " + tcp::net::last_socket_error(); return false; }

    tcp::net::set_reuse_address(listener_.get());

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        if (error) *error = "некорректный адрес: " + config_.bind_address;
        return false;
    }
    if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address),
               static_cast<tcp::net::socklen_type>(sizeof(address))) != 0) {
        if (error) *error = "bind(): " + tcp::net::last_socket_error();
        return false;
    }
    if (::listen(listener_.get(), SOMAXCONN) != 0) {
        if (error) *error = "listen(): " + tcp::net::last_socket_error();
        return false;
    }
    if (!tcp::net::set_nonblocking(listener_.get(), true, error)) return false;
    return true;
}

void Server::run() {
    LOG_INFO("сервер запущен ", config_.bind_address, ":", config_.port,
             " (max_conn=", config_.max_connections,
             ", idle=", config_.idle_timeout.count(), "ms",
             ", request=", config_.request_timeout.count(), "ms)");

    std::string error;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto waited = tcp::net::wait_readable(listener_.get(), 200ms, &error);
        if (waited == tcp::net::WaitResult::Timeout || waited == tcp::net::WaitResult::Interrupted) {
            reap_finished();
            continue;
        }
        if (waited == tcp::net::WaitResult::Error) {
            LOG_ERROR("poll(): ", error);
            break;
        }

        sockaddr_in peer{};
        auto peer_length = static_cast<tcp::net::socklen_type>(sizeof(peer));
        const tcp::socket_t client_fd =
            ::accept(listener_.get(), reinterpret_cast<sockaddr*>(&peer), &peer_length);

        if (!tcp::socket_valid(client_fd)) {
            const int code = tcp::net::last_socket_error_code();
            if (tcp::net::is_interrupted_error(code) || tcp::net::is_would_block_error(code)) continue;
            LOG_WARN("accept(): ", tcp::net::last_socket_error());
            continue;
        }

        const std::string peer_name = tcp::net::describe_peer(peer);
        const std::size_t active = active_connections_.load(std::memory_order_relaxed);
        const auto conn_id = ++g_conn_seq;

        if (active >= config_.max_connections) {
            LOG_WARN("[conn #", conn_id, "] лимит соединений (", active, "/",
                     config_.max_connections, "), отклонён ", peer_name);
            tcp::Socket rejected(client_fd);
            tcp::net::shutdown_socket(rejected.get());
            continue;
        }

        active_connections_.fetch_add(1, std::memory_order_relaxed);
        auto done = std::make_shared<std::atomic<bool>>(false);

        try {
            std::jthread worker([this, client_fd, peer_name, conn_id, done]() {
                try {
                    handle_client(client_fd, peer_name, conn_id);
                } catch (const std::exception& ex) {
                    LOG_ERROR("[conn #", conn_id, "] исключение: ", ex.what());
                } catch (...) {
                    LOG_ERROR("[conn #", conn_id, "] неизвестное исключение");
                }
                active_connections_.fetch_sub(1, std::memory_order_relaxed);
                done->store(true, std::memory_order_release);
            });
            std::lock_guard lock(workers_mutex_);
            workers_.push_back(Worker{std::move(done), std::move(worker)});
        } catch (const std::exception& ex) {
            LOG_ERROR("[conn #", conn_id, "] не удалось создать поток: ", ex.what());
            active_connections_.fetch_sub(1, std::memory_order_relaxed);
            tcp::Socket failed(client_fd);
        }
        reap_finished();
    }

    LOG_INFO("остановка: прекращаем приём новых подключений");
    listener_.reset();
    shutdown_all_connections();

    {
        std::lock_guard lock(workers_mutex_);
        workers_.clear();  // ~jthread дождётся завершения всех
    }
    LOG_INFO("сервер остановлен. соединений=", total_connections_.load(),
             ", запросов=", total_requests_.load(),
             ", ошибок=",   total_errors_.load());
}

void Server::handle_client(tcp::socket_t client_fd, std::string peer, std::uint64_t conn_id) {
    tcp::Socket socket(client_fd);
    const ConnectionGuard guard{this, &socket};
    register_connection(client_fd);
    total_connections_.fetch_add(1, std::memory_order_relaxed);

    LOG_INFO("[conn #", conn_id, "] подключился ", peer);

    if (!tcp::net::set_tcp_no_delay(socket.get()))
        LOG_DEBUG("[conn #", conn_id, "] TCP_NODELAY не установлен");

    std::string error;
    std::size_t requests = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {

        // 1) Ждём НАЧАЛО нового запроса не дольше idle_timeout.
        const auto idle_deadline = tcp::net::deadline_from_now(config_.idle_timeout);
        const auto waited = tcp::net::wait_readable_until(socket.get(), idle_deadline, nullptr);

        if (waited == tcp::net::WaitResult::Timeout) {
            LOG_INFO("[conn #", conn_id, "] idle timeout (",
                     config_.idle_timeout.count(), " ms) — закрываем");
            break;
        }
        if (waited == tcp::net::WaitResult::Error) {
            LOG_WARN("[conn #", conn_id, "] poll(): ошибка на сокете");
            break;
        }
        if (waited == tcp::net::WaitResult::Interrupted) continue;

        // 2) Читаем ПОЛНЫЙ кадр под request_deadline (защита от slowloris).
        const auto request_deadline = tcp::net::deadline_from_now(config_.request_timeout);
        std::string request;
        const auto status =
            tcp::protocol::recv_frame_until(socket.get(), request, request_deadline, &error);

        if (status == tcp::net::IoStatus::Closed) {
            LOG_INFO("[conn #", conn_id, "] клиент отключился (запросов: ", requests, ")");
            break;
        }
        if (status == tcp::net::IoStatus::Timeout) {
            LOG_WARN("[conn #", conn_id, "] request timeout — кадр не дочитан за ",
                     config_.request_timeout.count(), " ms (", error, ")");
            total_errors_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (status == tcp::net::IoStatus::Error) {
            LOG_WARN("[conn #", conn_id, "] ошибка чтения кадра: ", error);
            total_errors_.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        // 3) Валидация payload. Ошибка НЕ рвёт соединение — отвечаем ERROR.
        const auto kind = tcp::protocol::validate_request(request, config_.max_request_size);
        std::string response;
        if (kind == tcp::protocol::RequestError::Ok) {
            ++requests;
            total_requests_.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG("[conn #", conn_id, "] запрос #", requests, " (", request.size(),
                      " байт): \"", tcp::protocol::preview(request), "\"");
            response = tcp::protocol::make_ok_response(request);
        } else {
            total_errors_.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[conn #", conn_id, "] некорректный payload: ",
                     tcp::protocol::to_string(kind), " (размер=", request.size(), ")");
            response = tcp::protocol::make_error_response(kind);
        }

        // 4) Ответ тоже под дедлайном (backpressure на медленного читателя).
        const auto reply_deadline = tcp::net::deadline_from_now(config_.request_timeout);
        if (tcp::protocol::send_frame_until(socket.get(), response, reply_deadline, &error)
                != tcp::net::IoStatus::Ok) {
            LOG_WARN("[conn #", conn_id, "] ошибка отправки: ", error);
            total_errors_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    tcp::net::shutdown_socket(socket.get());
    LOG_INFO("[conn #", conn_id, "] соединение закрыто (запросов: ", requests, ")");
}

void Server::register_connection(tcp::socket_t fd) {
    std::lock_guard lock(connections_mutex_);
    connections_.push_back(fd);
}

void Server::release_connection(tcp::Socket& socket) {
    std::lock_guard lock(connections_mutex_);
    const tcp::socket_t fd = socket.get();
    connections_.erase(std::remove(connections_.begin(), connections_.end(), fd),
                       connections_.end());
    socket.reset();  // закрываем под мьютексом — исключаем гонку с shutdown
}

void Server::shutdown_all_connections() {
    std::lock_guard lock(connections_mutex_);
    if (!connections_.empty())
        LOG_INFO("инициировано закрытие активных соединений: ", connections_.size());
    for (tcp::socket_t fd : connections_) tcp::net::shutdown_socket(fd);
}

void Server::reap_finished() {
    std::lock_guard lock(workers_mutex_);
    std::erase_if(workers_, [](const Worker& worker) {
        return worker.done && worker.done->load(std::memory_order_acquire);
    });
}

}  // namespace

int main(int argc, char** argv) {
    tcp::WinsockGuard winsock;
    if (!winsock.ok()) { std::cerr << "Winsock init failed\n"; return 1; }

    Config config;
    if (!parse_args(argc, argv, config)) return 2;

    tcp::Logger::instance().set_level(config.log_level);
    if (!config.log_file.empty() && !tcp::Logger::instance().set_log_file(config.log_file))
        std::cerr << "Не удалось открыть файл лога: " << config.log_file << "\n";

#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::signal(SIGINT,  tcp_server_signal_handler);
    std::signal(SIGTERM, tcp_server_signal_handler);

    Server server(config);
    std::string error;
    if (!server.start(&error)) { LOG_ERROR("не удалось запустить: ", error); return 1; }

    server.run();
    return 0;
}
