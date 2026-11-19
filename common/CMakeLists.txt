add_library(tcp_common STATIC
    src/logger.cpp
    src/net_utils.cpp
    src/protocol.cpp
    src/socket_raii.cpp
)

add_library(tcp::common ALIAS tcp_common)

target_include_directories(tcp_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(tcp_common PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(tcp_common PUBLIC ws2_32)
endif()

tcp_enable_warnings(tcp_common)
