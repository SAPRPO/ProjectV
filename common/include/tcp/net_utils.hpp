#pragma once

#include "tcp/socket_raii.hpp"

#include <chrono>
#include <cstddef>
#include <string>

namespace tcp::net {

#ifdef _WIN32
using socklen_type = int;
#else
using socklen_type = socklen_t;
#endif

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/// Дедлайн «без ограничения».
inline TimePoint no_deadline() noexcept { return TimePoint::max(); }

TimePoint deadline_from_now(std::chrono::milliseconds timeout) noexcept;
/// Остаток до дедлайна; 0, если уже истёк; max, если дедлайн не задан.
std::chrono::milliseconds time_left(TimePoint deadline) noexcept;

enum class IoStatus { Ok, Closed, Timeout, Error };
enum class WaitResult { Ready, Timeout, Error, Interrupted };

std::string last_socket_error();
int last_socket_error_code() noexcept;
bool is_interrupted_error(int code) noexcept;
bool is_would_block_error(int code) noexcept;
bool is_timeout_error(int code) noexcept;
std::string describe_peer(const sockaddr_in& address);

bool set_reuse_address(socket_t fd, bool enable = true);
bool set_tcp_no_delay(socket_t fd, bool enable = true);
bool set_recv_timeout(socket_t fd, std::chrono::milliseconds timeout);
bool set_send_timeout(socket_t fd, std::chrono::milliseconds timeout);
bool set_nonblocking(socket_t fd, bool enable, std::string* error);

WaitResult wait_readable(socket_t fd, std::chrono::milliseconds timeout, std::string* error);
WaitResult wait_writable(socket_t fd, std::chrono::milliseconds timeout, std::string* error);

/// Дедлайн-версии. deadline == no_deadline() → блокируются бесконечно.
WaitResult wait_readable_until(socket_t fd, TimePoint deadline, std::string* error);
WaitResult wait_writable_until(socket_t fd, TimePoint deadline, std::string* error);

IoStatus read_exact_until(socket_t fd, void* buffer, std::size_t size,
                          TimePoint deadline, std::string* error);
IoStatus write_all_until(socket_t fd, const void* buffer, std::size_t size,
                         TimePoint deadline, std::string* error);

/// Обёртки без дедлайна (для юнит-тестов и служебного кода).
IoStatus read_exact(socket_t fd, void* buffer, std::size_t size, std::string* error);
IoStatus write_all(socket_t fd, const void* buffer, std::size_t size, std::string* error);

IoStatus connect_with_timeout(socket_t fd, const sockaddr* address, int address_length,
                              std::chrono::milliseconds timeout, std::string* error);

void shutdown_socket(socket_t fd) noexcept;

}  // namespace tcp::net
