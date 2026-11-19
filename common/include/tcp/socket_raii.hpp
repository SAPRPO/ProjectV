#pragma once

// Кроссплатформенная обёртка над сокетами:
//  * tcp::Socket      — RAII-владение дескриптором;
//  * tcp::WinsockGuard — инициализация/очистка Winsock (на POSIX — no-op).

#include <cstddef>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace tcp {

#ifdef _WIN32
using socket_t = SOCKET;
inline const socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

/// Корректен ли дескриптор (отличается от "невалидного").
bool socket_valid(socket_t socket_fd) noexcept;

/// Закрывает дескриптор, если он валиден (безопасно вызывать для невалидного).
void close_socket(socket_t socket_fd) noexcept;

/// Владеющий дескриптор сокета: закрывает его в деструкторе, перемещаемый,
/// но не копируемый.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(socket_t socket_fd) noexcept : fd_(socket_fd) {}
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    socket_t get() const noexcept { return fd_; }
    bool valid() const noexcept { return socket_valid(fd_); }
    explicit operator bool() const noexcept { return valid(); }

    /// Отдаёт владение наружу (дескриптор больше не закрывается).
    socket_t release() noexcept;

    /// Закрывает текущий дескриптор и начинает владеть новым.
    void reset(socket_t socket_fd = kInvalidSocket) noexcept;

private:
    socket_t fd_{kInvalidSocket};
};

/// Инициализация сетевой библиотеки (актуально только для Windows).
class WinsockGuard {
public:
    WinsockGuard() noexcept;
    ~WinsockGuard();
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;

    bool ok() const noexcept { return ok_; }

private:
    bool ok_{false};
};

}  // namespace tcp
