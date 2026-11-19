#include "tcp/socket_raii.hpp"

#include <utility>

namespace tcp {

bool socket_valid(socket_t socket_fd) noexcept {
#ifdef _WIN32
    return socket_fd != INVALID_SOCKET;
#else
    return socket_fd >= 0;
#endif
}

void close_socket(socket_t socket_fd) noexcept {
    if (!socket_valid(socket_fd)) {
        return;
    }
#ifdef _WIN32
    ::closesocket(socket_fd);
#else
    ::close(socket_fd);
#endif
}

Socket::~Socket() {
    reset();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

socket_t Socket::release() noexcept {
    const socket_t fd = fd_;
    fd_ = kInvalidSocket;
    return fd;
}

void Socket::reset(socket_t socket_fd) noexcept {
    if (fd_ != socket_fd) {
        close_socket(fd_);
    }
    fd_ = socket_fd;
}

WinsockGuard::WinsockGuard() noexcept {
#ifdef _WIN32
    WSADATA data{};
    ok_ = (::WSAStartup(MAKEWORD(2, 2), &data) == 0);
#else
    ok_ = true;
#endif
}

WinsockGuard::~WinsockGuard() {
#ifdef _WIN32
    if (ok_) {
        ::WSACleanup();
    }
#endif
}

}  // namespace tcp
