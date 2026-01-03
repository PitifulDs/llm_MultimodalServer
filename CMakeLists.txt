# CMakeLists.txt (project root)
cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)
project(EdgeLLMInfra LANGUAGES CXX)

# 先只挂 serving（不碰其他模块）

# add_subdirectory(network/src)

add_subdirectory(thirds/llama.cpp)
add_subdirectory(serving)
