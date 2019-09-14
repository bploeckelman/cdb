cmake_minimum_required(VERSION 3.14)
project(cdb C)

set(CMAKE_C_STANDARD 99)
set(ignoreMe "${CMAKE_CXX_COMPILER}")

add_executable(cdb main.c)