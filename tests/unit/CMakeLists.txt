cmake_minimum_required(VERSION 3.16)
project(unit_tests)

add_executable(http_utils_test http_utils_test.cpp)

target_include_directories(http_utils_test PRIVATE ../../)
