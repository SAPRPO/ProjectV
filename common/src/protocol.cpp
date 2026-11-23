#include "tcp/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace tcp::protocol {
namespace {
constexpr std::size_t kHeaderSize = 4;
}  // namespace

std::string encode_length(std::uint32_t length) {
    std::string header(kHeaderSize, '\0');
    header[0] = static_cast<char>((length >> 24) & 0xFFu);
    header[1] = static_cast<char>((length >> 16) & 0xFFu);
    header[2] = static_cast<char>((length >> 8) & 0xFFu);
    header[3] = static_cast<char>(length & 0xFFu);
    return header;
}

std::uint32_t decode_length(const char* header) {
    const auto byte = [header](std::size_t index) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(header[index]));
    };
    return (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
}

net::IoStatus send_frame_until(socket_t socket_fd, std::string_view payload,
                               net::TimePoint deadline, std::string* error) {
    if (payload.size() > kMaxPayloadSize) {
        if (error) *error = "сообщение слишком большое: " + std::to_string(payload.size()) + " байт";
        return net::IoStatus::Error;
    }
    const std::string header = encode_length(static_cast<std::uint32_t>(payload.size()));
    auto status = net::write_all_until(socket_fd, header.data(), header.size(), deadline, error);
    if (status != net::IoStatus::Ok) return status;
    if (payload.empty()) return net::IoStatus::Ok;
    return net::write_all_until(socket_fd, payload.data(), payload.size(), deadline, error);
}

net::IoStatus send_frame(socket_t socket_fd, std::string_view payload, std::string* error) {
    return send_frame_until(socket_fd, payload, net::no_deadline(), error);
}

net::IoStatus recv_frame_until(socket_t socket_fd, std::string& payload,
                               net::TimePoint deadline, std::string* error) {
    char header[kHeaderSize] = {0};
    auto status = net::read_exact_until(socket_fd, header, kHeaderSize, deadline, error);
    if (status != net::IoStatus::Ok) return status;

    const std::uint32_t length = decode_length(header);
    if (length > kMaxPayloadSize) {
        if (error) *error = "сообщение слишком большое: " + std::to_string(length) + " байт";
        return net::IoStatus::Error;
    }
    payload.assign(length, '\0');
    if (length == 0) return net::IoStatus::Ok;

    status = net::read_exact_until(socket_fd, &payload[0], length, deadline, error);
    if (status != net::IoStatus::Ok) { payload.clear(); return status; }
    return net::IoStatus::Ok;
}

net::IoStatus recv_frame(socket_t socket_fd, std::string& payload, std::string* error) {
    return recv_frame_until(socket_fd, payload, net::no_deadline(), error);
}

std::string_view to_string(RequestError error) noexcept {
    switch (error) {
        case RequestError::Ok:           return "Ok";
        case RequestError::Empty:        return "Empty";
        case RequestError::TooLarge:     return "TooLarge";
        case RequestError::NotPrintable: return "NotPrintable";
    }
    return "Unknown";
}

RequestError validate_request(std::string_view payload, std::size_t max_length) noexcept {
    if (payload.empty())             return RequestError::Empty;
    if (payload.size() > max_length) return RequestError::TooLarge;
    for (char ch : payload) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 && byte != '\t' && byte != '\r' && byte != '\n')
            return RequestError::NotPrintable;
        if (byte == 0x7F) return RequestError::NotPrintable;
    }
    return RequestError::Ok;
}

std::string make_ok_response(std::string_view request) {
    std::ostringstream out;
    out << "OK " << request.size() << " bytes: " << to_upper(request);
    return out.str();
}

std::string make_error_response(RequestError error) {
    return "ERROR " + std::string(to_string(error));
}

// --- обратная совместимость ---

std::string make_response(std::string_view request) {
    return make_ok_response(request);
}

std::string to_upper(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char ch : text) {
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return result;
}

bool is_quit_command(std::string_view text) {
    std::string command;
    command.reserve(text.size());
    for (char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (std::isspace(byte)) continue;
        command.push_back(static_cast<char>(std::tolower(byte)));
    }
    return command == "quit" || command == "exit" || command == "q" || command == ":q";
}

std::string preview(std::string_view text, std::size_t max_length) {
    const std::size_t limit = std::min(text.size(), max_length);
    std::string result;
    result.reserve(limit + 16);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        switch (byte) {
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (byte < 0x20 || byte == 0x7F) {
                    char buffer[8] = {0};
                    std::snprintf(buffer, sizeof(buffer), "\\x%02X", byte);
                    result += buffer;
                } else {
                    result.push_back(static_cast<char>(byte));
                }
        }
    }
    if (text.size() > limit) result += "...";
    return result;
}

}  // namespace tcp::protocol
