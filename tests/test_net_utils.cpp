#include "test_framework.hpp"
#include "tcp/net_utils.hpp"
#include "tcp/socket_raii.hpp"
// добавляем в tests/test_net_utils.cpp
#include "tcp/protocol.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool make_pair(tcp::Socket& a, tcp::Socket& b, std::string& error) {
    tcp::Socket lst(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    tcp::net::set_reuse_address(lst.get());

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(lst.get(), reinterpret_cast<sockaddr*>(&addr),
               static_cast<tcp::net::socklen_type>(sizeof(addr))) != 0) {
        error = tcp::net::last_socket_error(); return false;
    }
    if (::listen(lst.get(), 1) != 0) { error = tcp::net::last_socket_error(); return false; }
    auto len = static_cast<tcp::net::socklen_type>(sizeof(addr));
    ::getsockname(lst.get(), reinterpret_cast<sockaddr*>(&addr), &len);

    tcp::Socket c(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (::connect(c.get(), reinterpret_cast<sockaddr*>(&addr),
                  static_cast<tcp::net::socklen_type>(sizeof(addr))) != 0) {
        error = tcp::net::last_socket_error(); return false;
    }
    tcp::Socket s(::accept(lst.get(), nullptr, nullptr));
    if (!s.valid()) { error = tcp::net::last_socket_error(); return false; }

    a = std::move(c); b = std::move(s);
    return true;
}

}  // namespace

TCP_TEST_CASE(deadline_from_now_zero_is_expired) {
    const auto d = tcp::net::deadline_from_now(0ms);
    TCP_CHECK(tcp::net::time_left(d).count() == 0);
}

TCP_TEST_CASE(no_deadline_is_max) {
    TCP_CHECK(tcp::net::no_deadline() == tcp::net::TimePoint::max());
    TCP_CHECK(tcp::net::time_left(tcp::net::no_deadline()).count() > 0);
}

TCP_TEST_CASE(read_exact_until_times_out_without_data) {
    tcp::Socket a, b;
    std::string error;
    TCP_REQUIRE(make_pair(a, b, error));

    char buf[4] = {0};
    const auto deadline = tcp::net::deadline_from_now(150ms);
    const auto started = std::chrono::steady_clock::now();
    const auto st = tcp::net::read_exact_until(b.get(), buf, sizeof(buf), deadline, &error);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    TCP_CHECK(st == tcp::net::IoStatus::Timeout);
    TCP_CHECK(elapsed < 2s);
    TCP_CHECK(!error.empty());
}

TCP_TEST_CASE(read_exact_until_times_out_on_partial_frame) {
    tcp::Socket a, b;
    std::string error;
    TCP_REQUIRE(make_pair(a, b, error));

    const char partial[2] = {'x', 'y'};
    TCP_REQUIRE(tcp::net::write_all(a.get(), partial, 2, &error) == tcp::net::IoStatus::Ok);

    char buf[8] = {0};
    const auto deadline = tcp::net::deadline_from_now(150ms);
    const auto st = tcp::net::read_exact_until(b.get(), buf, sizeof(buf), deadline, &error);
    TCP_CHECK(st == tcp::net::IoStatus::Timeout);  // 2 байта получили, остальное — нет
}

TCP_TEST_CASE(write_all_until_times_out_on_full_buffer) {
    tcp::Socket a, b;
    std::string error;
    TCP_REQUIRE(make_pair(a, b, error));

    // Не читаем из b: заполним буфер отправителя
    const std::string big(8 * 1024 * 1024, 'x');
    const auto deadline = tcp::net::deadline_from_now(200ms);
    const auto st = tcp::net::write_all_until(a.get(), big.data(), big.size(), deadline, &error);
    TCP_CHECK(st == tcp::net::IoStatus::Timeout);
}



TCP_TEST_CASE(recv_frame_until_times_out_on_partial_header) {
    tcp::Socket a, b;
    std::string error;
    TCP_REQUIRE(make_pair(a, b, error));

    // Отправляем только половину 4-байтового заголовка
    const std::string header = tcp::protocol::encode_length(10);
    TCP_REQUIRE(tcp::net::write_all(a.get(), header.data(), 2, &error)
                == tcp::net::IoStatus::Ok);

    const auto deadline = tcp::net::deadline_from_now(150ms);
    std::string payload;
    TCP_CHECK(tcp::protocol::recv_frame_until(b.get(), payload, deadline, &error)
              == tcp::net::IoStatus::Timeout);
    TCP_CHECK(payload.empty());
}

int main() {
    testfw::run_all();
    return 0;
}