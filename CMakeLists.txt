cmake_minimum_required(VERSION 3.10)

project(MyProject)

# Стандарт C++
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Вариант 1: явное указание исходников (рекомендуется)
add_executable(MyProject 
src/main.cpp
src/print.cpp)

# Подключаем папку include для заголовочных файлов
target_include_directories(MyProject PRIVATE include)