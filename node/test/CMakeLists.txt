cmake_minimum_required(VERSION 3.12)
project(test)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_subdirectory(../../thirds/llama.cpp ${CMAKE_BINARY_DIR}/llama)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../..
    ../../hybrid-comm/include
    ../../infra-controller/include
    ../../engine
    ../../serving
    ../../utils
    ../../thirds/llama.cpp/include
    ../../thirds/llama.cpp/ggml/include
)

file(GLOB SRCS 
    src/main.cpp
    src/llm_task.cc
    src/llm_server.cc
    ../../engine/LlamaEngine.cc
    ../../engine/ModelContext.cc
)

add_executable(${PROJECT_NAME} ${SRCS})

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    pthread
    ${CMAKE_SOURCE_DIR}/../../install/lib/libstackflow.a
    zmq
    glog
    llama
)
