cmake_minimum_required(VERSION 3.16)
project(unit_tests)

add_executable(http_utils_test http_utils_test.cpp)
target_include_directories(http_utils_test PRIVATE ../../)

find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY sqlite3)
find_library(GLOG_LIBRARY glog)

if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "sqlite3 development files not found. Please install libsqlite3-dev.")
endif()

if(NOT GLOG_LIBRARY)
    message(FATAL_ERROR "glog library not found.")
endif()

add_executable(rag_test
    rag_test.cpp
)
target_include_directories(rag_test PRIVATE ../../ ${SQLITE3_INCLUDE_DIR})
target_link_libraries(rag_test PRIVATE serving_http serving_core ${SQLITE3_LIBRARY} ${GLOG_LIBRARY})
