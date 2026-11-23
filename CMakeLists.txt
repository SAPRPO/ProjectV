cmake_minimum_required(VERSION 3.20)
project(tcp_project VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(TCP_BUILD_TESTS "Собирать тесты" ON)
# "address,undefined" ИЛИ "thread" — вместе нельзя
option(TCP_SANITIZE "" "Включить санитайзеры (address,undefined|thread)")

find_package(Threads REQUIRED)

if(TCP_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=${TCP_SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${TCP_SANITIZE})
endif()

function(tcp_enable_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_definitions(${target} PRIVATE
        _POSIX_C_SOURCE=200809L
        _DEFAULT_SOURCE)
endfunction()

add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)

if(TCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
