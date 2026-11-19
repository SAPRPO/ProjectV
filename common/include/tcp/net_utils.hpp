#pragma once

#include "tcp/socket_raii.hpp"

#include <chrono>
#include <cstddef>
#include <string>

namespace tcp::net {

/// Тип длины адреса (в POSIX это socklen_t, в Winsock — int).
#ifdef _WIN32
using socklen_type = int;
#else
using socklen_type = socklen_t;
#endif

/// Результат операций ввода-вывода.
enum class IoStatus {
    Ok,
    Closed,   ///< пир корректно закрыл соединение на границе сообщения
    Timeout,  ///< не дождались данных за отведённое время
    Error,
};

/// Результат ожидания готовности сокета.
enum class WaitResult { Ready, Timeout, Error, Interrupted };

std::string last_socket_error();        ///< "code=111 (Connection refused)"
int last_socket_error_code() noexcept;  ///< errno / WSAGetLastError()

bool is_interrupted_error(int code) noexcept;
bool is_would_block_error(int code) noexcept;
bool is_timeout_error(int code) noexcept;

std::string describe_peer(const sockaddr_in& address);  ///< "127.0.0.1:54321"

// --- настройка сокетов -------------------------------------------------------
bool set_reuse_address(socket_t socket_fd, bool enable = true);
bool set_tcp_no_delay(socket_t socket_fd, bool enable = true);
bool set_recv_timeout(socket_t socket_fd, std::chrono::milliseconds timeout);
bool set_send_timeout(socket_t socket_fd, std::chrono::milliseconds timeout);
bool set_nonblocking(socket_t socket_fd, bool enable, std::string* error);

// --- ожидание готовности -----------------------------------------------------
WaitResult wait_readable(socket_t socket_fd, std::chrono::milliseconds timeout, std::string* error);
WaitResult wait_writable(socket_t socket_fd, std::chrono::milliseconds timeout, std::string* error);

// --- блокирующий ввод-вывод --------------------------------------------------
IoStatus read_exact(socket_t socket_fd, void* buffer, std::size_t size, std::string* error);
IoStatus write_all(socket_t socket_fd, const void* buffer, std::size_t size, std::string* error);

/// Неблокирующее подключение с таймаутом (сокет после вызова — блокирующий).
IoStatus connect_with_timeout(socket_t socket_fd, const sockaddr* address, int address_length,
                              std::chrono::milliseconds timeout, std::string* error);

/// Мягкое закрытие: разбудить пир и наш собственный блокирующий вызов.
void shutdown_socket(socket_t socket_fd) noexcept;

}  // namespace tcp::net
