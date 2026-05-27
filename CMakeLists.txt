cmake_minimum_required(VERSION 3.10)
project(DistributedIM_Gateway CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 添加项目头文件搜索路径
include_directories(include)

# 手动查找 hiredis 库
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if(NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIBRARY)
    message(FATAL_ERROR "Could not find hiredis library. Please install it (e.g., sudo apt-get install libhiredis-dev) or set HIREDIS_INCLUDE_DIR and HIREDIS_LIBRARY manually.")
endif()

# 将 hiredis 的头文件路径添加到项目中
include_directories(${HIREDIS_INCLUDE_DIR})

# --- FetchContent for nlohmann/json ---
include(FetchContent)

FetchContent_Declare(
  json_external
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.2 # 使用一个稳定的版本号，你也可以选择其他版本
)

FetchContent_MakeAvailable(json_external)

# 将 nlohmann/json 的头文件路径添加到项目中
# target_include_directories 更好，但为了简单起见，这里使用 include_directories
# 实际项目中，更推荐 target_include_directories(gateway_server PRIVATE ${json_external_SOURCE_DIR}/include)
include_directories(${json_external_SOURCE_DIR}/include)
# --- End FetchContent for nlohmann/json ---


# 定义源文件
set(SOURCES
    src/main.cpp
    src/EpollServer.cpp
    src/RedisClient.cpp
    src/JsonParser.cpp
    src/MessageHandler.cpp
)

# 添加可执行文件
add_executable(gateway_server ${SOURCES})

# 链接库
# target_link_libraries(gateway_server PRIVATE hiredis::hiredis) # 原始行
target_link_libraries(gateway_server PRIVATE ${HIREDIS_LIBRARY})