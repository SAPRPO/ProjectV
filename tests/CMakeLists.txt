# Заголовок с мини-фреймворком доступен всем тестам (без сторонних зависимостей).
add_library(tcp_tests_common INTERFACE)
target_include_directories(tcp_tests_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcp_tests_common INTERFACE tcp_common)

# Для GCC < 9 нужна явная линковка std::filesystem
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(tcp_tests_common INTERFACE stdc++fs)
endif()

function(tcp_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE tcp_tests_common)
    tcp_enable_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

tcp_add_test(test_protocol test_protocol.cpp)
tcp_add_test(test_logger test_logger.cpp)

# Интеграционный тест сам запускает сервер, путь к которому передаёт CMake.
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE tcp_tests_common)
tcp_enable_warnings(test_integration)
add_test(NAME test_integration COMMAND test_integration $<TARGET_FILE:tcp_server>)
set_tests_properties(test_integration PROPERTIES TIMEOUT 180)
