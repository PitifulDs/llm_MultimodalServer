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

target_include_directories(llm_demo PRIVATE
    ../node/llm
    ../thirds/llama.cpp/include
)

target_link_libraries(llm_demo PRIVATE llama)
