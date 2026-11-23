# tests/CMakeLists.txt
#
# Тестовый набор ProjectV.
#
# ВАЖНО: файла test_framework.cpp в проекте НЕТ. Поэтому общий main
# сюда не подключается. Поддержаны два варианта:
#
#   1) каждый test_*.cpp содержит свой main          → TCP_TEST_MAIN_SOURCE = ""
#   2) main лежит в отдельном общем файле           → TCP_TEST_MAIN_SOURCE = "имя.cpp"
#
# По умолчанию выбран (1).

# Если найдёте общий файл с main (например test_main.cpp) — впишите его имя сюда.
set(TCP_TEST_MAIN_SOURCE "")

# ---------------------------------------------------------------------------
# tcp_add_test(test_protocol)   ← ОДИН аргумент, без ".cpp"
# ---------------------------------------------------------------------------
function(tcp_add_test name)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp")
        message(FATAL_ERROR
            "tcp_add_test(${name}): файл '${name}.cpp' не найден в "
            "${CMAKE_CURRENT_SOURCE_DIR}. Имя теста должно совпадать с "
            "именем .cpp без расширения.")
    endif()

    set(_sources "${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp")

    if(TCP_TEST_MAIN_SOURCE)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TCP_TEST_MAIN_SOURCE}")
            message(FATAL_ERROR
                "tcp_add_test(${name}): общий файл с main "
                "'${TCP_TEST_MAIN_SOURCE}' не найден в "
                "${CMAKE_CURRENT_SOURCE_DIR}.")
        endif()
        list(APPEND _sources "${CMAKE_CURRENT_SOURCE_DIR}/${TCP_TEST_MAIN_SOURCE}")
    endif()

    add_executable(${name} ${_sources})

    if(TARGET Threads::Threads)
        target_link_libraries(${name} PRIVATE tcp_common Threads::Threads)
    else()
        target_link_libraries(${name} PRIVATE tcp_common)
    endif()

    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(COMMAND tcp_enable_warnings)
        tcp_enable_warnings(${name})
    endif()

    add_test(NAME ${name} COMMAND ${name})

    if(EXISTS "${CMAKE_BINARY_DIR}/bin")
        set_tests_properties(${name} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Регистрация тестов — ОДИН аргумент на строку
# ---------------------------------------------------------------------------
tcp_add_test(test_logger)
tcp_add_test(test_protocol)
tcp_add_test(test_net_utils)
tcp_add_test(test_integration)

# ---------------------------------------------------------------------------
# GCC < 9 и <filesystem>
# ---------------------------------------------------------------------------
if(UNIX
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    foreach(t test_logger test_protocol test_net_utils test_integration)
        if(TARGET ${t})
            target_link_libraries(${t} PRIVATE stdc++fs)
        endif()
    endforeach()
endif()
