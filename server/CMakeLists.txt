add_executable(tcp_server main.cpp)
target_link_libraries(tcp_server PRIVATE tcp_common)
tcp_enable_warnings(tcp_server)
 
