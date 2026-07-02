cmake_minimum_required(VERSION 3.10)
project(DistributedIM CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 添加子目录
add_subdirectory(gateway)

# 输出项目信息
message(STATUS "DistributedIM project configured")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX standard: ${CMAKE_CXX_STANDARD}")
