#include "test_framework.hpp"
#include "tcp/logger.hpp"
#include "tcp/net_utils.hpp"
#include "tcp/protocol.hpp"
#include "tcp/socket_raii.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32

int main(int, char**) {
    std::cout << "[ SKIP ] интеграционный тест недоступен на Windows "
                 "(управление процессом сервера не реализовано)\n";
    return 0;
}

#else  // ------------------------------------------------------------------ POSIX

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

std::string g_server_path;
std::uint16_t g_port = 0;

/// Дочерний процесс сервера (fork/exec + ожидание завершения).
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { terminate_and_wait(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool start(const std::string& program, std::uint16_t port, const std::string& log_path) {
        log_path_ = log_path;
        const int log_fd = ::open(log_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (log_fd < 0) {
            return false;
        }

        std::vector<std::string> storage{program, "--port", std::to_string(port), "--bind",
                                         "127.0.0.1", "--log-level", "debug"};
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& item : storage) {
            argv.push_back(const_cast<char*>(item.c_str()));
        }
        argv.push_back(nullptr);

        pid_ = ::fork();
        if (pid_ < 0) {
            ::close(log_fd);
            return false;
        }
        if (pid_ == 0) {
            ::setsid();
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
            ::execv(program.c_str(), argv.data());
            ::_exit(127);
        }

        ::close(log_fd);
        return true;
    }

    bool wait_for_exit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            const pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return true;
            }
            if (result < 0) {
                pid_ = -1;
                return false;
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    bool signal_and_wait_for_exit(std::chrono::milliseconds timeout) {
        if (pid_ <= 0) {
            return true;
        }
        ::kill(pid_, SIGTERM);
        return wait_for_exit(timeout);
    }

    void terminate_and_wait() {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        if (!wait_for_exit(5000ms)) {
            ::kill(pid_, SIGKILL);
            wait_for_exit(2000ms);
        }
    }

private:
    pid_t pid_{-1};
    std::string log_path_;
};

ChildProcess g_server;

std::string server_log_path() {
    return (std::filesystem::temp_directory_path() / "tcp_project_integration.log").string();
}

void print_log_tail(const std::string& path, std::size_t max_chars = 4000) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return;
    }
    std::ostringstream content;
    content << input.rdbuf();
    const std::string text = content.str();
    const std::size_t start = text.size() > max_chars ? text.size() - max_chars : 0;
    std::cerr << "----- лог сервера (хвост) -----\n" << text.substr(start) << "-------------------------------\n";
}

// --- сетевые помощники -------------------------------------------------------

std::uint16_t pick_free_port() {
    tcp::Socket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!socket.valid()) {
        return 0;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (::bind(socket.get(), reinterpret_cast<sockaddr*>(&address),
               static_cast<tcp::net::socklen_type>(sizeof(address))) != 0) {
        return 0;
    }

    auto length = static_cast<tcp::net::socklen_type>(sizeof(address));
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

bool connect_client(std::uint16_t port, tcp::Socket& socket, std::string& error) {
    tcp::Socket candidate(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!candidate.valid()) {
        error = "socket(): " + tcp::net::last_socket_error();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const auto status = tcp::net::connect_with_timeout(
        candidate.get(), reinterpret_cast<sockaddr*>(&address),
        static_cast<int>(sizeof(address)), 1000ms, &error);
    if (status != tcp::net::IoStatus::Ok) {
        return false;
    }

    socket = std::move(candidate);
    return true;
}

bool wait_for_server(std::uint16_t port, std::chrono::milliseconds timeout, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        tcp::Socket probe;
        if (connect_client(port, probe, error)) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

/// Полный цикл «подключиться — отправить — получить ответ — закрыть».
bool roundtrip(std::uint16_t port, const std::string& request, std::string& response,
               std::string& error) {
    tcp::Socket socket;
    if (!connect_client(port, socket, error)) {
        return false;
    }
    if (tcp::protocol::send_frame(socket.get(), request, &error) != tcp::net::IoStatus::Ok) {
        return false;
    }
    if (tcp::protocol::recv_frame(socket.get(), response, &error) != tcp::net::IoStatus::Ok) {
        return false;
    }
    tcp::net::shutdown_socket(socket.get());
    return true;
}

// --- запуск общего сервера для тестов ---------------------------------------

bool start_shared_server(std::string& error) {
    g_port = pick_free_port();
    if (g_port == 0) {
        error = "не удалось подобрать свободный порт";
        return false;
    }

    const std::string log_path = server_log_path();
    if (!g_server.start(g_server_path, g_port, log_path)) {
        error = "не удалось запустить процесс сервера";
        return false;
    }
    if (!wait_for_server(g_port, 10s, error)) {
        print_log_tail(log_path);
        error = "сервер не отвечает на порту " + std::to_string(g_port) + ": " + error;
        return false;
    }
    return true;
}

bool ensure_server(std::string& error) {
    static std::string first_error;
    static const bool started = [] {
        std::string local_error;
        const bool ok = start_shared_server(local_error);
        first_error = local_error;
        return ok;
    }();

    if (!started) {
        error = first_error;
    }
    return started;
}

}  // namespace

// --- тесты -------------------------------------------------------------------

TCP_TEST_CASE(server_is_reachable) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));
}

TCP_TEST_CASE(single_request_response) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    std::string response;
    TCP_REQUIRE(roundtrip(g_port, "hello", response, error));
    TCP_CHECK_EQ(response, tcp::protocol::make_response("hello"));
}
/*
TCP_TEST_CASE(empty_request_response) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    std::string response;
    TCP_REQUIRE(roundtrip(g_port, "", response, error));
    TCP_CHECK_EQ(response, tcp::protocol::make_response(""));
}*/
/*
TCP_TEST_CASE(large_request_response) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    const std::string request(100 * 1024, 'z');  // 100 KiB
    std::string response;
    TCP_REQUIRE(roundtrip(g_port, request, response, error));
    TCP_CHECK_EQ(response, tcp::protocol::make_response(request));
}*/

TCP_TEST_CASE(multiple_simultaneous_connections) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    constexpr int kClients = 8;
    constexpr int kRequestsPerClient = 10;

    std::atomic<int> failures{0};
    std::vector<std::thread> clients;

    for (int client_index = 0; client_index < kClients; ++client_index) {
        clients.emplace_back([client_index, &failures] {
            std::string local_error;
            std::string response;
            tcp::Socket socket;
            if (!connect_client(g_port, socket, local_error)) {
                ++failures;
                return;
            }
            for (int request_index = 0; request_index < kRequestsPerClient; ++request_index) {
                const std::string request =
                    "клиент " + std::to_string(client_index) + ", запрос " + std::to_string(request_index);
                if (tcp::protocol::send_frame(socket.get(), request, &local_error) !=
                    tcp::net::IoStatus::Ok) {
                    ++failures;
                    return;
                }
                if (tcp::protocol::recv_frame(socket.get(), response, &local_error) !=
                    tcp::net::IoStatus::Ok) {
                    ++failures;
                    return;
                }
                if (response != tcp::protocol::make_response(request)) {
                    ++failures;
                    return;
                }
            }
        });
    }

    for (auto& client : clients) {
        client.join();
    }
    TCP_CHECK_EQ(failures.load(), 0);
}

TCP_TEST_CASE(abrupt_disconnect_is_handled) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    // 1. Несколько последовательных подключений
    for (int i = 0; i < 5; ++i) {
        std::string response;
        TCP_REQUIRE(roundtrip(g_port, "ping", response, error));
        TCP_CHECK_EQ(response, tcp::protocol::make_response("ping"));
    }

    // 2. Резкий разрыв: отправляем запрос и закрываем сокет, не читая ответ
    {
        tcp::Socket socket;
        TCP_REQUIRE(connect_client(g_port, socket, error));
        TCP_REQUIRE(tcp::protocol::send_frame(socket.get(), "abrupt", &error) ==
                    tcp::net::IoStatus::Ok);
        // socket закрывается деструктором
    }

    // 3. Сервер обязан остаться работоспособным
    std::string response;
    TCP_REQUIRE(roundtrip(g_port, "after abrupt close", response, error));
    TCP_CHECK_EQ(response, tcp::protocol::make_response("after abrupt close"));
}

TCP_TEST_CASE(server_terminates_on_sigterm) {
    const std::uint16_t port = pick_free_port();
    TCP_REQUIRE(port != 0);

    ChildProcess server;
    const std::string log_path =
        (std::filesystem::temp_directory_path() / "tcp_project_integration_shutdown.log").string();
    TCP_REQUIRE(server.start(g_server_path, port, log_path));

    std::string error;
    TCP_REQUIRE(wait_for_server(port, 10s, error));

    const auto started_at = std::chrono::steady_clock::now();
    TCP_CHECK(server.signal_and_wait_for_exit(5000ms));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    TCP_CHECK(elapsed < 5000ms);

}
/*
TCP_TEST_CASE(idle_client_is_disconnected) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    tcp::Socket socket;
    TCP_REQUIRE(connect_client(g_port, socket, error));

    // Ничего не отправляем — ждём, пока сервер сам разорвёт по idle_timeout.
    // Сервер запущен с --idle-timeout; в тестовом запуске ставим короткий через аргументы.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    char probe[1] = {0};
    tcp::net::IoStatus status = tcp::net::IoStatus::Ok;
    while (std::chrono::steady_clock::now() < deadline) {
        status = tcp::net::read_exact_until(socket.get(), probe, 1,
                                            std::chrono::steady_clock::now() + 500ms, &error);
        if (status == tcp::net::IoStatus::Closed) break;
    }
    TCP_CHECK(status == tcp::net::IoStatus::Closed);  // сервер инициировал закрытие
}*/

TCP_TEST_CASE(slowloris_partial_frame_is_disconnected) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    tcp::Socket socket;
    TCP_REQUIRE(connect_client(g_port, socket, error));

    // Объявили 100 байт, отправили 10, замолчали — сервер обязан закрыть по request_timeout.
    const std::string header = tcp::protocol::encode_length(100);
    TCP_REQUIRE(tcp::net::write_all(socket.get(), header.data(), header.size(), &error)
                == tcp::net::IoStatus::Ok);
    const std::string partial(10, 'a');
    TCP_REQUIRE(tcp::net::write_all(socket.get(), partial.data(), partial.size(), &error)
                == tcp::net::IoStatus::Ok);

    const auto deadline = std::chrono::steady_clock::now() + 30s;
    char probe[1] = {0};
    tcp::net::IoStatus status = tcp::net::IoStatus::Ok;
    while (std::chrono::steady_clock::now() < deadline) {
        status = tcp::net::read_exact_until(socket.get(), probe, 1,
                                            std::chrono::steady_clock::now() + 500ms, &error);
        if (status == tcp::net::IoStatus::Closed) break;
    }
    TCP_CHECK(status == tcp::net::IoStatus::Closed);
}

TCP_TEST_CASE(invalid_payload_gets_error_and_connection_survives) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    tcp::Socket socket;
    TCP_REQUIRE(connect_client(g_port, socket, error));

    // Мусор (управляющий байт) => ERROR, соединение НЕ рвётся
    const std::string garbage = std::string("\x01\x02\x03", 3);
    TCP_REQUIRE(tcp::protocol::send_frame(socket.get(), garbage, &error) == tcp::net::IoStatus::Ok);
    std::string response;
    TCP_REQUIRE(tcp::protocol::recv_frame(socket.get(), response, &error) == tcp::net::IoStatus::Ok);
    TCP_CHECK(response.starts_with("ERROR ")); // TODO: check this

    // Следующий валидный запрос — успешен в том же сокете
    TCP_REQUIRE(tcp::protocol::send_frame(socket.get(), "hello", &error) == tcp::net::IoStatus::Ok);
    TCP_REQUIRE(tcp::protocol::recv_frame(socket.get(), response, &error) == tcp::net::IoStatus::Ok);
    TCP_CHECK_EQ(response, tcp::protocol::make_ok_response("hello"));
}

TCP_TEST_CASE(abrupt_rst_does_not_kill_server) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    {
        tcp::Socket socket;
        TCP_REQUIRE(connect_client(g_port, socket, error));
        TCP_REQUIRE(tcp::protocol::send_frame(socket.get(), "abrupt", &error) == tcp::net::IoStatus::Ok);

        linger lin{};
        lin.l_onoff = 1;
        lin.l_linger = 0;  // RST вместо FIN
        ::setsockopt(socket.get(), SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
        // socket закрывается с RST
    }

    // Сервер должен остаться живым:
    std::string response;
    TCP_REQUIRE(roundtrip(g_port, "after-rst", response, error));
    TCP_CHECK_EQ(response, tcp::protocol::make_ok_response("after-rst"));
}

TCP_TEST_CASE(no_fd_leak_after_many_connections) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    for (int i = 0; i < 200; ++i) {
        std::string response;
        TCP_REQUIRE(roundtrip(g_port, "ping " + std::to_string(i), response, error));
    }
    // Косвенная проверка: если бы fd протекали, сервер бы отказал по лимиту соединений.
    std::string response;
    TCP_REQUIRE(roundtrip(g_port, "final", response, error));
    TCP_CHECK_EQ(response, tcp::protocol::make_ok_response("final"));
}

TCP_TEST_CASE(idle_client_is_disconnected) {
    std::string error;
    TCP_REQUIRE(ensure_server(error));

    tcp::Socket socket;
    TCP_REQUIRE(connect_client(g_port, socket, error));

    const auto deadline = std::chrono::steady_clock::now() + 30s;
    char probe[1] = {0};
    tcp::net::IoStatus status = tcp::net::IoStatus::Ok;
    while (std::chrono::steady_clock::now() < deadline) {
        status = tcp::net::read_exact_until(socket.get(), probe, 1,
                                            std::chrono::steady_clock::now() + 500ms, &error);
        if (status == tcp::net::IoStatus::Closed) break;
    }
    TCP_CHECK(status == tcp::net::IoStatus::Closed);
}

//TCP_TEST_CASE(slowloris_partial_frame_is_disconnected) {
//    std::string error;
//    TCP_REQUIRE(ensure_server(error));
//
//    tcp::Socket socket;
//    TCP_REQUIRE(connect_client(g_port, socket, error));
//
//    const std::string header = tcp::protocol::encode_length(100);
//    TCP_REQUIRE(tcp::net::write_all(socket.get(), header.data(), header.size(), &error)
//                == tcp::net::IoStatus::Ok);
//    const std::string partial(10, 'a');
//    TCP_REQUIRE(tcp::net::write_all(socket.get(), partial.data(), partial.size(), &error)
//                == tcp::net::IoStatus::Ok);
//
//    const auto deadline = std::chrono::steady_clock::now() + 30s;
//    char probe[1] = {0};
//    tcp::net::IoStatus status = tcp::net::IoStatus::Ok;
//    while (std::chrono::steady_clock::now() < deadline) {
//        status = tcp::net::read_exact_until(socket.get(), probe, 1,
//                                            std::chrono::steady_clock::now() + 500ms, &error);
//        if (status == tcp::net::IoStatus::Closed) break;
//    }
//    TCP_CHECK(status == tcp::net::IoStatus::Closed);
//}

//TCP_TEST_CASE(invalid_payload_gets_error_and_connection_survives) {
//    std::string error;
//    TCP_REQUIRE(ensure_server(error));
//
//    tcp::Socket socket;
//    TCP_REQUIRE(connect_client(g_port, socket, error));
//
//    const std::string garbage("\x01\x02\x03", 3);
//    TCP_REQUIRE(tcp::protocol::send_frame(socket.get(), garbage, &error) == tcp::net::IoStatus::Ok);
//    std::string response;
//    TCP_REQUIRE(tcp::protocol::recv_frame(socket.get(), response, &error) == tcp::net::IoStatus::Ok);
//    TCP_CHECK(response.starts_with("ERROR "));
//
//    TCP_REQUIRE(tcp::protocol::send_frame(socket.get(), "hello", &error) == tcp::net::IoStatus::Ok);
//    TCP_REQUIRE(tcp::protocol::recv_frame(socket.get(), response, &error) == tcp::net::IoStatus::Ok);
//    TCP_CHECK_EQ(response, tcp::protocol::make_ok_response("hello"));
//}
//
//TCP_TEST_CASE(abrupt_rst_does_not_kill_server) {
//    std::string error;
//    TCP_REQUIRE(ensure_server(error));
//
//    {
//        tcp::Socket socket;
//        TCP_REQUIRE(connect_client(g_port, socket, error));
//        TCP_REQUIRE(tcp::protocol::send_frame(socket.get(), "abrupt", &error)
//                    == tcp::net::IoStatus::Ok);
//
//        linger lin{};
//        lin.l_onoff = 1;
//        lin.l_linger = 0;
//        ::setsockopt(socket.get(), SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
//    }
//
//    std::string response;
//    TCP_REQUIRE(roundtrip(g_port, "after-rst", response, error));
//    TCP_CHECK_EQ(response, tcp::protocol::make_ok_response("after-rst"));
//}

//TCP_TEST_CASE(no_fd_leak_after_many_connections) {
//    std::string error;
//    TCP_REQUIRE(ensure_server(error));
//
//    for (int i = 0; i < 200; ++i) {
//        std::string response;
//        TCP_REQUIRE(roundtrip(g_port, "ping " + std::to_string(i), response, error));
//    }
//    std::string response;
//    TCP_REQUIRE(roundtrip(g_port, "final", response, error));
//    TCP_CHECK_EQ(response, tcp::protocol::make_ok_response("final"));
//}


int main(int argc, char** argv) {
    tcp::Logger::instance().set_level(tcp::LogLevel::Warn);

    if (argc < 2) {
        std::cerr << "Использование: " << argv[0] << " <путь-к-tcp_server>\n";
        return 2;
    }
    g_server_path = argv[1];

    if (!std::filesystem::exists(g_server_path)) {
        std::cerr << "Не найден исполняемый файл сервера: " << g_server_path << "\n";
        return 2;
    }

    return testfw::run_all();
}

#endif  // _WIN32
