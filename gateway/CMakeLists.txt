cmake_minimum_required(VERSION 3.10)
project(DistributedIM_Gateway CXX)

set(CMAKE_CXX_STANDARD 23)
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

include_directories(${HIREDIS_INCLUDE_DIR})

# 查找 OpenSSL（WebSocket 握手 SHA1 计算需要）
find_package(OpenSSL REQUIRED)

# 可选：查找 libsodium（E2EE 加密需要）
find_library(SODIUM_LIBRARY sodium)
if(SODIUM_LIBRARY)
    set(E2EE_ENABLED 1)
    add_definitions(-DE2E_ENABLED)
    message(STATUS "libsodium found, E2EE encryption enabled")
else()
    message(STATUS "libsodium not found, E2EE encryption disabled")
endif()

# 可选：查找 libcurl（AI 服务客户端需要）
find_package(CURL QUIET)
if(CURL_FOUND)
    add_definitions(-DAI_SERVICE_ENABLED)
    message(STATUS "libcurl found, AI service client enabled")
else()
    message(STATUS "libcurl not found, AI service client disabled")
endif()

# --- 第三方库依赖 ---
# 使用 find_package 查找系统安装的库，避免每次从 GitHub 下载
# 安装依赖: sudo apt install nlohmann-json3-dev libspdlog-dev

# nlohmann/json（header-only，在 Ubuntu 22.04+ 中为 nlohmann-json3-dev）
find_package(nlohmann_json 3.11.0 REQUIRED)

# spdlog（header-only 模式，使用 C++23 std::format 避免 fmt 版本冲突）
add_definitions(-DSPDLOG_HEADER_ONLY -DSPDLOG_USE_STD_FORMAT)
find_package(spdlog 1.12.0 REQUIRED)

# googletest（仅在构建测试时需要，仍使用 FetchContent）
if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
      googletest_external
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.15.2
    )
endif()

# 定义源文件（v2.0 新增：WebSocketCodec, GroupManager, AckTracker, RateLimiter, PriorityQueue）
set(SOURCES
    src/main.cpp
    src/EpollServer.cpp
    src/RedisClient.cpp
    src/JsonParser.cpp
    src/MessageHandler.cpp
    src/Logger.cpp
    src/Config.cpp
    src/WebSocketCodec.cpp
    src/GroupManager.cpp
    src/AckTracker.cpp
    src/RateLimiter.cpp
    src/PriorityQueue.cpp
    src/GatewayRegistry.cpp
    src/Metrics.cpp
)

# 条件编译：E2EE 加密模块
if(E2EE_ENABLED)
    list(APPEND SOURCES src/E2EECrypto.cpp)
endif()

# 条件编译：AI 服务客户端和审计日志（需要 libcurl）
if(CURL_FOUND)
    list(APPEND SOURCES src/AIServiceClient.cpp src/AuditLogger.cpp)
endif()

# 添加主可执行文件
add_executable(gateway_server ${SOURCES})

# 链接库
target_link_libraries(gateway_server PRIVATE
    ${HIREDIS_LIBRARY}
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    OpenSSL::SSL
    OpenSSL::Crypto
)

# 条件链接 libsodium（E2EE）
if(E2EE_ENABLED)
    target_link_libraries(gateway_server PRIVATE ${SODIUM_LIBRARY})
endif()

# 条件链接 libcurl（AI 服务）
if(CURL_FOUND)
    target_link_libraries(gateway_server PRIVATE CURL::libcurl)
endif()

# 添加配置测试可执行文件
add_executable(test_config_load tools/test_config_load.cpp src/Config.cpp src/Logger.cpp)
target_link_libraries(test_config_load PRIVATE nlohmann_json::nlohmann_json)

# 添加客户端测试可执行文件
add_executable(test_client tools/test_client.cpp)
target_link_libraries(test_client PRIVATE)

# 添加配置错误处理测试可执行文件
add_executable(test_config_error tools/test_config_error.cpp src/Config.cpp)
target_link_libraries(test_config_error PRIVATE nlohmann_json::nlohmann_json)

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
    target_link_libraries(gateway_tests PRIVATE ${HIREDIS_LIBRARY} nlohmann_json::nlohmann_json spdlog::spdlog_header_only gtest gtest_main)

    enable_testing()
    add_test(NAME gateway_tests COMMAND gateway_tests)
endif()
