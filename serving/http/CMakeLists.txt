cmake_minimum_required(VERSION 3.10)

find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY sqlite3)

if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "sqlite3 development files not found. Please install libsqlite3-dev.")
endif()

# =====================================
# 1.serving_core：ServingContext / Engine
# =====================================
add_library(serving_core STATIC
    ${CMAKE_SOURCE_DIR}/engine/DummyEngine.cc
    ${CMAKE_SOURCE_DIR}/engine/RpcEngine.cc
    ${CMAKE_SOURCE_DIR}/engine/EngineFactory.cc
    ${CMAKE_SOURCE_DIR}/engine/ModelRegistry.cc
    ${CMAKE_SOURCE_DIR}/engine/StackFlowEngine.cc
    ${CMAKE_SOURCE_DIR}/engine/LlamaEngine.cc
    ${CMAKE_SOURCE_DIR}/engine/ModelContext.cc
    ${CMAKE_SOURCE_DIR}/serving/core/SessionManager.cc
    ${CMAKE_SOURCE_DIR}/serving/core/RedisSessionStore.cc
    # ${CMAKE_SOURCE_DIR}/serving/core/Session.cc
    ${CMAKE_SOURCE_DIR}/serving/core/EngineExecutor.cc
    ${CMAKE_SOURCE_DIR}/serving/core/agent/AgentExecutor.cc
    ${CMAKE_SOURCE_DIR}/serving/core/agent/AgentParser.cc
    ${CMAKE_SOURCE_DIR}/serving/core/agent/AgentPrompt.cc
    ${CMAKE_SOURCE_DIR}/serving/core/agent/ToolRegistry.cc
    ${CMAKE_SOURCE_DIR}/serving/core/agent/BuiltinTools.cc
    ${CMAKE_SOURCE_DIR}/serving/core/ThreadPool.cc
    ${CMAKE_SOURCE_DIR}/serving/core/SessionExecutor.cc
    ${CMAKE_SOURCE_DIR}/serving/rag/SqliteIndexStore.cc
    ${CMAKE_SOURCE_DIR}/serving/rag/Retriever.cc
    ${CMAKE_SOURCE_DIR}/serving/rag/PromptAssembler.cc
    ${CMAKE_SOURCE_DIR}/serving/rag/RAGExecutor.cc
)

target_include_directories(serving_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${SQLITE3_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/thirds/llama.cpp/include
        ${CMAKE_SOURCE_DIR}/thirds/llama.cpp/ggml/include
)

target_link_libraries(serving_core
    PRIVATE
        llama
        ${SQLITE3_LIBRARY}
)

target_compile_features(serving_core
    PUBLIC cxx_std_17
)

# =====================================
# 2. serving_http 静态库（核心逻辑）
# =====================================
add_library(serving_http STATIC
    ChatRequestParser.cc
    HttpGateway.cc
    HttpStreamSession.cc
    OpenAIStreamWriter.cc 
    StackFlowsClient.cc
    NetworkHttpServer.cc
)

target_include_directories(serving_http
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/network/include
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
