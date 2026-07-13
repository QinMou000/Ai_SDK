cmake_minimum_required(VERSION 3.23)

project(ai_sdk VERSION 0.1.0 LANGUAGES CXX)

option(AISDK_BUILD_EXAMPLES "构建示例目录入口" ON)
option(AISDK_BUILD_TESTS "构建自动化测试" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(ai_sdk
    src/core/Config.cpp
    src/core/Message.cpp
    src/core/ChatRequest.cpp
    src/core/ChatResponse.cpp
    src/provider/DeepSeekProvider.cpp
    src/http/HttpClient.cpp
    src/http/SSEParser.cpp
    src/tool/Tool.cpp
    src/tool/ToolRegistry.cpp
    src/tool/ToolExecutor.cpp
    src/AIClient.cpp
)

target_include_directories(ai_sdk
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ai_sdk
    PUBLIC
        cpr::cpr
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

target_compile_features(ai_sdk PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(ai_sdk PRIVATE /W4 /utf-8)
else()
    target_compile_options(ai_sdk PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(AISDK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AISDK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
