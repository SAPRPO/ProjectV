#include "test_framework.hpp"
#include "tcp/net_utils.hpp"
#include "tcp/protocol.hpp"
#include "tcp/socket_raii.hpp"

#include <cstdint>
#include <string>
#include <thread>

namespace {

struct SocketPair {
    tcp::Socket client;
    tcp::Socket server;
};

/// Создаёт пару соединённых сокетов через loopback TCP (порт выбирается ядром).
bool make_socket_pair(SocketPair& pair, std::string& error) {
    tcp::Socket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener.valid()) {
        error = "socket(): " + tcp::net::last_socket_error();
        return false;
    }
    tcp::net::set_reuse_address(listener.get());

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;  // эфемерный порт

    if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address),
               static_cast<tcp::net::socklen_type>(sizeof(address))) != 0) {
        error = "bind(): " + tcp::net::last_socket_error();
        return false;
    }
    if (::listen(listener.get(), 1) != 0) {
        error = "listen(): " + tcp::net::last_socket_error();
        return false;
    }

    auto length = static_cast<tcp::net::socklen_type>(sizeof(address));
    if (::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        error = "getsockname(): " + tcp::net::last_socket_error();
        return false;
    }

    tcp::Socket client(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!client.valid()) {
        error = "socket(): " + tcp::net::last_socket_error();
        return false;
    }
    if (::connect(client.get(), reinterpret_cast<sockaddr*>(&address),
                  static_cast<tcp::net::socklen_type>(sizeof(address))) != 0) {
        error = "connect(): " + tcp::net::last_socket_error();
        return false;
    }

    tcp::Socket server(::accept(listener.get(), nullptr, nullptr));
    if (!server.valid()) {
        error = "accept(): " + tcp::net::last_socket_error();
        return false;
    }

    pair.client = std::move(client);
    pair.server = std::move(server);
    return true;
}

}  // namespace

TCP_TEST_CASE(length_header_roundtrip) {
    TCP_CHECK_EQ(tcp::protocol::encode_length(0), std::string("\0\0\0\0", 4));
    TCP_CHECK_EQ(tcp::protocol::encode_length(0x01020304u), std::string("\x01\x02\x03\x04", 4));
    TCP_CHECK_EQ(tcp::protocol::decode_length("\x01\x02\x03\x04"), 0x01020304u);
    TCP_CHECK_EQ(tcp::protocol::decode_length("\xFF\xFF\xFF\xFF"), 0xFFFFFFFFu);
}

TCP_TEST_CASE(frame_roundtrip_ascii) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    const std::string payload = "Hello, TCP!";
    TCP_CHECK(tcp::protocol::send_frame(pair.client.get(), payload, &error) ==
              tcp::net::IoStatus::Ok);

    std::string received;
    TCP_CHECK(tcp::protocol::recv_frame(pair.server.get(), received, &error) ==
              tcp::net::IoStatus::Ok);
    TCP_CHECK_EQ(received, payload);
}

TCP_TEST_CASE(frame_roundtrip_empty) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    TCP_CHECK(tcp::protocol::send_frame(pair.client.get(), "", &error) == tcp::net::IoStatus::Ok);

    std::string received = "непусто";
    TCP_CHECK(tcp::protocol::recv_frame(pair.server.get(), received, &error) ==
              tcp::net::IoStatus::Ok);
    TCP_CHECK_EQ(received, std::string());
}

TCP_TEST_CASE(frame_roundtrip_binary) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    std::string payload;
    for (int value = 0; value < 256; ++value) {
        payload.push_back(static_cast<char>(value));
    }

    TCP_CHECK(tcp::protocol::send_frame(pair.client.get(), payload, &error) ==
              tcp::net::IoStatus::Ok);

    std::string received;
    TCP_CHECK(tcp::protocol::recv_frame(pair.server.get(), received, &error) ==
              tcp::net::IoStatus::Ok);
    TCP_CHECK_EQ(received.size(), payload.size());
    TCP_CHECK(received == payload);  // в том числе нулевые байты
}

TCP_TEST_CASE(frame_roundtrip_large) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    const std::string payload(tcp::protocol::kMaxPayloadSize, 'x');  // 1 MiB

    tcp::net::IoStatus send_status = tcp::net::IoStatus::Error;
    std::thread sender([&] {
        send_status = tcp::protocol::send_frame(pair.client.get(), payload, &error);
    });

    std::string received;
    const auto recv_status = tcp::protocol::recv_frame(pair.server.get(), received, &error);
    sender.join();

    TCP_CHECK(send_status == tcp::net::IoStatus::Ok);
    TCP_CHECK(recv_status == tcp::net::IoStatus::Ok);
    TCP_CHECK_EQ(received.size(), payload.size());
    TCP_CHECK(received == payload);
}

TCP_TEST_CASE(oversized_frame_is_rejected) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    const std::uint32_t too_big = tcp::protocol::kMaxPayloadSize + 1u;
    const std::string header = tcp::protocol::encode_length(too_big);
    TCP_REQUIRE(tcp::net::write_all(pair.client.get(), header.data(), header.size(), &error) ==
                tcp::net::IoStatus::Ok);

    std::string payload;
    TCP_CHECK(tcp::protocol::recv_frame(pair.server.get(), payload, &error) ==
              tcp::net::IoStatus::Error);
    TCP_CHECK(!error.empty());
}

TCP_TEST_CASE(closed_peer_is_detected) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    pair.client.reset();  // корректное закрытие клиентской стороны

    std::string payload;
    TCP_CHECK(tcp::protocol::recv_frame(pair.server.get(), payload, &error) ==
              tcp::net::IoStatus::Closed);
}

TCP_TEST_CASE(incomplete_frame_is_error) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    const std::string header = tcp::protocol::encode_length(8).substr(0, 2);  // 2 байта из 4
    TCP_REQUIRE(tcp::net::write_all(pair.client.get(), header.data(), header.size(), &error) ==
                tcp::net::IoStatus::Ok);
    pair.client.reset();

    std::string payload;
    TCP_CHECK(tcp::protocol::recv_frame(pair.server.get(), payload, &error) ==
              tcp::net::IoStatus::Error);
}

TCP_TEST_CASE(oversized_send_is_rejected) {
    SocketPair pair;
    std::string error;
    TCP_REQUIRE(make_socket_pair(pair, error));

    const std::string payload(tcp::protocol::kMaxPayloadSize + 1u, 'y');
    TCP_CHECK(tcp::protocol::send_frame(pair.client.get(), payload, &error) ==
              tcp::net::IoStatus::Error);
}

TCP_TEST_CASE(application_layer_helpers) {
    TCP_CHECK_EQ(tcp::protocol::make_response("abc"), std::string("OK 3 bytes: ABC"));
    TCP_CHECK_EQ(tcp::protocol::make_response(""), std::string("OK 0 bytes: "));
    TCP_CHECK_EQ(tcp::protocol::to_upper("Hello, World! 123"), std::string("HELLO, WORLD! 123"));

    TCP_CHECK(tcp::protocol::is_quit_command("quit"));
    TCP_CHECK(tcp::protocol::is_quit_command("  EXIT "));
    TCP_CHECK(tcp::protocol::is_quit_command(":q"));
    TCP_CHECK(!tcp::protocol::is_quit_command("quitter"));
    TCP_CHECK(!tcp::protocol::is_quit_command("hello"));

    TCP_CHECK_EQ(tcp::protocol::preview("a\nb\tc"), std::string("a\\nb\\tc"));
    TCP_CHECK_EQ(tcp::protocol::preview("abcdef", 3), std::string("abc..."));
}

int main() {
    return testfw::run_all();
}
