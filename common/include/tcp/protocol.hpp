#pragma once

#include "tcp/net_utils.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tcp::protocol {

inline constexpr std::uint32_t kMaxPayloadSize = 1024u * 1024u;

// --- фрейминг ---
net::IoStatus send_frame(socket_t socket_fd, std::string_view payload,
                         std::string* error = nullptr);
net::IoStatus recv_frame(socket_t socket_fd, std::string& payload,
                         std::string* error = nullptr);

// --- фрейминг с дедлайном ---
net::IoStatus send_frame_until(socket_t socket_fd, std::string_view payload,
                               net::TimePoint deadline, std::string* error = nullptr);
net::IoStatus recv_frame_until(socket_t socket_fd, std::string& payload,
                               net::TimePoint deadline, std::string* error = nullptr);

std::string encode_length(std::uint32_t length);
std::uint32_t decode_length(const char* header);

// --- валидация прикладного payload ---
enum class RequestError { Ok, Empty, TooLarge, NotPrintable };
std::string_view to_string(RequestError error) noexcept;
RequestError validate_request(std::string_view payload,
                              std::size_t max_length = 64 * 1024) noexcept;

// --- ответы ---
std::string make_ok_response(std::string_view request);      // "OK <N> bytes: <UPPER>"
std::string make_error_response(RequestError error);         // "ERROR <code>"

// --- обратная совместимость ---
std::string make_response(std::string_view request);         // == make_ok_response

// --- прочее ---
std::string to_upper(std::string_view text);
bool is_quit_command(std::string_view text);
std::string preview(std::string_view text, std::size_t max_length = 64);

}  // namespace tcp::protocol
