# serving/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(serving)
set(CMAKE_CXX_STANDARD 17)

# serving 是一个“聚合目录”，本身不生成 target
# 只负责把各个 northbound adapter 挂进来

add_subdirectory(${CMAKE_SOURCE_DIR}/../network/src ${CMAKE_BINARY_DIR}/network)
add_subdirectory(http)
