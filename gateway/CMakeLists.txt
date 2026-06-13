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

# --- FetchContent for dependencies ---
include(FetchContent)

# nlohmann/json
FetchContent_Declare(
  json_external
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.2
)

# spdlog
FetchContent_Declare(
  spdlog_external
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
)

# googletest
FetchContent_Declare(
  googletest_external
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)

FetchContent_MakeAvailable(json_external spdlog_external)

# 将依赖的头文件路径添加到项目中
include_directories(${json_external_SOURCE_DIR}/include)
include_directories(${spdlog_external_SOURCE_DIR}/include)

# --- End FetchContent ---

# 定义源文件
set(SOURCES
    src/main.cpp
    src/EpollServer.cpp
    src/RedisClient.cpp
    src/JsonParser.cpp
    src/MessageHandler.cpp
    src/Logger.cpp
    src/Config.cpp
)

# 添加主可执行文件
add_executable(gateway_server ${SOURCES})
target_link_libraries(gateway_server PRIVATE ${HIREDIS_LIBRARY} spdlog::spdlog)

# 添加配置测试可执行文件
add_executable(test_config_load tools/test_config_load.cpp src/Config.cpp src/Logger.cpp)
target_link_libraries(test_config_load PRIVATE spdlog::spdlog)

# 添加客户端测试可执行文件
add_executable(test_client tools/test_client.cpp)
target_link_libraries(test_client PRIVATE)

# 添加配置错误处理测试可执行文件
add_executable(test_config_error tools/test_config_error.cpp src/Config.cpp)
target_link_libraries(test_config_error PRIVATE)

# --- Tests ---
option(BUILD_TESTS "Build tests" OFF)

if(BUILD_TESTS)
    FetchContent_MakeAvailable(googletest_external)
    include_directories(${googletest_external_SOURCE_DIR}/googletest/include)
    
    set(TEST_SOURCES
        tests/test_redis_client.cpp
        tests/test_config.cpp
        tests/test_json_parser.cpp
    )
    
    add_executable(gateway_tests ${TEST_SOURCES} src/RedisClient.cpp src/Config.cpp src/JsonParser.cpp src/Logger.cpp)
    target_link_libraries(gateway_tests PRIVATE ${HIREDIS_LIBRARY} spdlog::spdlog gtest gtest_main)
    
    enable_testing()
    add_test(NAME gateway_tests COMMAND gateway_tests)
endif()
