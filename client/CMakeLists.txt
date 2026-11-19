add_executable(tcp_client main.cpp)
target_link_libraries(tcp_client PRIVATE tcp_common)
tcp_enable_warnings(tcp_client)
 
