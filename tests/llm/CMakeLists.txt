cmake_minimum_required(VERSION 3.16)
project(llm_test)

add_executable(llm_test llm_test.cpp)

# llama.cpp
add_subdirectory(
    ../../thirds/llama.cpp
    ${CMAKE_BINARY_DIR}/llama
)

target_link_libraries(llm_test PRIVATE llama)
target_include_directories(llm_test PRIVATE ../../thirds/llama.cpp/include)
