#include "tcp/net_utils.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  include <mstcpip.h>
#  define TCP_POLL WSAPoll
#else
#  include <fcntl.h>
#  include <poll.h>
#  define TCP_POLL ::poll
#endif

namespace tcp::net {
namespace {

constexpr std::size_t kMaxChunkSize = 0x7FFFFFFF;  // ограничение int для recv/send

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // не получать SIGPIPE при разрыве
#else
constexpr int kSendFlags = 0;
#endif

int poll_timeout_ms(std::chrono::milliseconds timeout) {
    const auto count = timeout.count();
    if (count < 0) {
        return -1;  // бесконечно
    }
    if (count > 0x7FFFFFFF) {
        return 0x7FFFFFFF;
    }
    return static_cast<int>(count);
}

WaitResult wait_event(socket_t socket_fd, short events, std::chrono::milliseconds timeout,
                      std::string* error) {
    pollfd descriptor{};
    descriptor.fd = socket_fd;
    descriptor.events = events;
    descriptor.revents = 0;

    int result = 0;
    do {
        result = TCP_POLL(&descriptor, 1, poll_timeout_ms(timeout));
    } while (result < 0 && is_interrupted_error(last_socket_error_code()));

    if (result == 0) {
        return WaitResult::Timeout;
    }
    if (result < 0) {
        if (error) {
            *error = "poll(): " + last_socket_error();
        }
        return WaitResult::Error;
    }

    const short revents = descriptor.revents;
    if (revents & (POLLERR | POLLNVAL)) {
        if (error) {
            *error = "poll(): ошибка сокета";
        }
        return WaitResult::Error;
    }
    if (revents & (events | POLLHUP)) {
        return WaitResult::Ready;  // при POLLHUP последующий recv вернёт 0 (EOF)
    }
    return WaitResult::Timeout;
}

}  // namespace

std::string last_socket_error() {
    const int code = last_socket_error_code();
    std::ostringstream out;
    out << "code=" << code << " (" << std::error_code(code, std::system_category()).message() << ")";
    return out.str();
}

int last_socket_error_code() noexcept {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool is_interrupted_error(int code) noexcept {
#ifdef _WIN32
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

bool is_would_block_error(int code) noexcept {
#ifdef _WIN32
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS;
#endif
}

bool is_timeout_error(int code) noexcept {
#ifdef _WIN32
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == ETIMEDOUT;
#endif
}

std::string describe_peer(const sockaddr_in& address) {
    char text[INET_ADDRSTRLEN] = {0};
    // в Winsock inet_ntop принимает не-const указатель, в POSIX — const
    auto* raw = const_cast<in_addr*>(&address.sin_addr);
    const char* converted = ::inet_ntop(AF_INET, raw, text, sizeof(text));

    std::ostringstream out;
    out << (converted != nullptr ? text : std::string("unknown")) << ':'
        << static_cast<unsigned>(ntohs(address.sin_port));
    return out.str();
}

bool set_reuse_address(socket_t socket_fd, bool enable) {
    const int value = enable ? 1 : 0;
    return ::setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

bool set_tcp_no_delay(socket_t socket_fd, bool enable) {
    const int value = enable ? 1 : 0;
    return ::setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

bool set_recv_timeout(socket_t socket_fd, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeout.count());
    return ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0;
#endif
}

bool set_send_timeout(socket_t socket_fd, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeout.count());
    return ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
#else
    timeval value{};
    value.tv_sec = static_cast<long>(timeout.count() / 1000);
    value.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    return ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0;
#endif
}

bool set_nonblocking(socket_t socket_fd, bool enable, std::string* error) {
#ifdef _WIN32
    u_long mode = enable ? 1u : 0u;
    if (::ioctlsocket(socket_fd, FIONBIO, &mode) != 0) {
        if (error) {
            *error = "ioctlsocket(FIONBIO): " + last_socket_error();
        }
        return false;
    }
    return true;
#else
    const int flags = ::fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0) {
        if (error) {
            *error = "fcntl(F_GETFL): " + last_socket_error();
        }
        return false;
    }
    const int updated = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(socket_fd, F_SETFL, updated) < 0) {
        if (error) {
            *error = "fcntl(F_SETFL): " + last_socket_error();
        }
        return false;
    }
    return true;
#endif
}

WaitResult wait_readable(socket_t socket_fd, std::chrono::milliseconds timeout, std::string* error) {
    return wait_event(socket_fd, POLLIN, timeout, error);
}

WaitResult wait_writable(socket_t socket_fd, std::chrono::milliseconds timeout, std::string* error) {
    return wait_event(socket_fd, POLLOUT, timeout, error);
}

IoStatus read_exact(socket_t socket_fd, void* buffer, std::size_t size, std::string* error) {
    auto* data = static_cast<char*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const std::size_t remaining = size - total;
        const int chunk = static_cast<int>(std::min(remaining, kMaxChunkSize));
        const long long received = static_cast<long long>(::recv(socket_fd, data + total, chunk, 0));

        if (received > 0) {
            total += static_cast<std::size_t>(received);
            continue;
        }
        if (received == 0) {
            if (total == 0) {
                if (error) {
                    *error = "пир закрыл соединение";
                }
                return IoStatus::Closed;
            }
            if (error) {
                *error = "пир закрыл соединение в середине сообщения";
            }
            return IoStatus::Error;
        }

        const int code = last_socket_error_code();
        if (is_interrupted_error(code)) {
            continue;
        }
        if (is_timeout_error(code)) {
            if (error) {
                *error = "таймаут чтения";
            }
            return IoStatus::Timeout;
        }
        if (error) {
            *error = "recv(): " + last_socket_error();
        }
        return IoStatus::Error;
    }
    return IoStatus::Ok;
}

IoStatus write_all(socket_t socket_fd, const void* buffer, std::size_t size, std::string* error) {
    const auto* data = static_cast<const char*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const std::size_t remaining = size - total;
        const int chunk = static_cast<int>(std::min(remaining, kMaxChunkSize));
        const long long sent =
            static_cast<long long>(::send(socket_fd, data + total, chunk, kSendFlags));

        if (sent > 0) {
            total += static_cast<std::size_t>(sent);
            continue;
        }

        const int code = last_socket_error_code();
        if (is_interrupted_error(code)) {
            continue;
        }
        if (is_timeout_error(code)) {
            if (error) {
                *error = "таймаут записи";
            }
            return IoStatus::Timeout;
        }
        if (error) {
            *error = "send(): " + last_socket_error();
        }
        return IoStatus::Error;
    }
    return IoStatus::Ok;
}

IoStatus connect_with_timeout(socket_t socket_fd, const sockaddr* address, int address_length,
                              std::chrono::milliseconds timeout, std::string* error) {
    if (!set_nonblocking(socket_fd, true, error)) {
        return IoStatus::Error;
    }

    const int result = ::connect(socket_fd, address, static_cast<socklen_type>(address_length));
    if (result == 0) {
        set_nonblocking(socket_fd, false, nullptr);
        return IoStatus::Ok;
    }

    if (!is_would_block_error(last_socket_error_code())) {
        if (error) {
            *error = "connect(): " + last_socket_error();
        }
        set_nonblocking(socket_fd, false, nullptr);
        return IoStatus::Error;
    }

    const WaitResult waited = wait_writable(socket_fd, timeout, error);
    if (waited == WaitResult::Timeout) {
        if (error) {
            *error = "connect(): превышен таймаут подключения";
        }
        set_nonblocking(socket_fd, false, nullptr);
        return IoStatus::Timeout;
    }
    if (waited != WaitResult::Ready) {
        set_nonblocking(socket_fd, false, nullptr);
        return IoStatus::Error;
    }

    int socket_error = 0;
    socklen_type length = sizeof(socket_error);
    if (::getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&socket_error), &length) != 0) {
        if (error) {
            *error = "getsockopt(SO_ERROR): " + last_socket_error();
        }
        set_nonblocking(socket_fd, false, nullptr);
        return IoStatus::Error;
    }
    set_nonblocking(socket_fd, false, nullptr);

    if (socket_error != 0) {
        if (error) {
            std::ostringstream out;
            out << "connect(): code=" << socket_error << " ("
                << std::error_code(socket_error, std::system_category()).message() << ")";
            *error = out.str();
        }
        return IoStatus::Error;
    }
    return IoStatus::Ok;
}

void shutdown_socket(socket_t socket_fd) noexcept {
    if (!socket_valid(socket_fd)) {
        return;
    }
#ifdef _WIN32
    ::shutdown(socket_fd, SD_BOTH);
#else
    ::shutdown(socket_fd, SHUT_RDWR);
#endif
}

}  // namespace tcp::net
