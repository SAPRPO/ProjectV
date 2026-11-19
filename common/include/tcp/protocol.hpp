#pragma once

#include "tcp/net_utils.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tcp::protocol {

/// Максимальный размер одного сообщения (payload), байт.
inline constexpr std::uint32_t kMaxPayloadSize = 1024u * 1024u;  // 1 MiB

// Формат сообщения: [4 байта длины, big-endian][payload произвольных байт].
// Позволяет передавать бинарные данные и строки, содержащие '\0'.

/// Отправляет один кадр.
net::IoStatus send_frame(socket_t socket_fd, std::string_view payload,
                         std::string* error = nullptr);

/// Принимает один кадр.
/// Возвращает net::IoStatus::Closed, если пир закрыл соединение на границе кадра.
net::IoStatus recv_frame(socket_t socket_fd, std::string& payload, std::string* error = nullptr);

/// Кодирование/декодирование 4-байтового заголовка (используется тестами).
std::string encode_length(std::uint32_t length);
std::uint32_t decode_length(const char* header);

// --- прикладной уровень ------------------------------------------------------

/// Ответ сервера: "OK <N> bytes: <ЗАПРОС В ВЕРХНЕМ РЕГИСТРЕ>".
std::string make_response(std::string_view request);

/// ASCII-перевод в верхний регистр (байты >= 0x80 не изменяются).
std::string to_upper(std::string_view text);

/// "quit" / "exit" / "q" / ":q" (пробелы игнорируются, регистр не важен).
bool is_quit_command(std::string_view text);

/// Безопасное представление строки для логов (экранирование + обрезка).
std::string preview(std::string_view text, std::size_t max_length = 64);

}  // namespace tcp::protocol
