cmake_minimum_required(VERSION 3.16)
project(llm_demo)

set(CMAKE_CXX_STANDARD 17)

# llama.cpp
add_subdirectory(
    ../thirds/llama.cpp
    ${CMAKE_BINARY_DIR}/llama
)

add_executable(llm_demo
    llm_demo.cpp
    ../node/llm/LLMUnit.cpp
)

add_executable(llm_server
    llm_server.cpp
    ../node/llm/LLMUnit.cpp
)

# 公共的 include 目录
target_include_directories(llm_demo PRIVATE
    ../node/llm
    ../thirds/llama.cpp/include
)

target_include_directories(llm_server PRIVATE
    ../node/llm
    ../thirds/llama.cpp/include
)

# 链接 llama 库（llama.cpp 子目录里生成的 target）
target_link_libraries(llm_demo PRIVATE llama)
target_link_libraries(llm_server PRIVATE llama)
