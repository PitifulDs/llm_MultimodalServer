cmake_minimum_required(VERSION 3.10)

# =====================================
# 1.serving_core：ServingContext / Engine
# =====================================
add_library(serving_core STATIC
    ${CMAKE_SOURCE_DIR}/../engine/DummyEngine.cc
    ${CMAKE_SOURCE_DIR}/../engine/RpcEngine.cc
)

target_include_directories(serving_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/..
)

target_compile_features(serving_core
    PUBLIC cxx_std_17
)

# =====================================
# 2. serving_http 静态库（核心逻辑）
# =====================================
add_library(serving_http STATIC
    HttpGateway.cc
    HttpStreamSession.cc
    StackFlowsClient.cc
    NetworkHttpServer.cc
)

target_include_directories(serving_http
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/..
        ${CMAKE_SOURCE_DIR}/../network/include
)

target_compile_features(serving_http
    PUBLIC cxx_std_17
)

target_link_libraries(serving_http
    PRIVATE
        serving_core
        network
)

# =====================================
# 3. HTTP Server 可执行文件（进程入口）
# =====================================
add_executable(serving_http_server
    http_main.cc
)

target_compile_features(serving_http_server
    PRIVATE cxx_std_17
)

target_link_libraries(serving_http_server
    PRIVATE
        serving_http
        pthread
)

set_target_properties(serving_http_server PROPERTIES
    OUTPUT_NAME "serving_http_server"
)


