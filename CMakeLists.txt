cmake_minimum_required(VERSION 3.16)

project(tcp_project
    VERSION 1.0.0
    DESCRIPTION "Пример TCP-клиента и TCP-сервера на C++17"
    LANGUAGES CXX)

# ---------------------------------------------------------------- настройки --
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Тип сборки" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Бинарники и библиотеки складываем в build/bin и build/lib
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(TCP_BUILD_TESTS "Собирать тесты" ON)
option(TCP_WARNINGS_AS_ERRORS "Считать предупреждения ошибками" OFF)

find_package(Threads REQUIRED)

# ---------------------------------------------------- строгие предупреждения --
function(tcp_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            WIN32_LEAN_AND_MEAN
            NOMINMAX)
        if(TCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(TCP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ------------------------------------------------------------------ подкаталоги --
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)

if(TCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -------------------------------------------------------------------- итоги --
message(STATUS "tcp_project: тип сборки = ${CMAKE_BUILD_TYPE}")
message(STATUS "tcp_project: тесты        = ${TCP_BUILD_TESTS}")
