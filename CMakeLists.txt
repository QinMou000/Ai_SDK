cmake_minimum_required(VERSION 3.23)

project(ai_sdk VERSION 0.1.0 LANGUAGES CXX)

option(AISDK_BUILD_EXAMPLES "构建示例目录入口" ON)
option(AISDK_BUILD_TESTS "构建自动化测试" ON)
option(AISDK_BUILD_MCP "构建独立 MCP Client、传输与适配层" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

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
    src/trace/Trace.cpp
    src/trace/TraceRecorder.cpp
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
        Threads::Threads
)

target_compile_features(ai_sdk PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(ai_sdk PRIVATE /W4 /utf-8)
else()
    target_compile_options(ai_sdk PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(AISDK_BUILD_MCP)
    set(AISDK_MCP_PLATFORM_SOURCE src/mcp/process/PosixProcess.cpp)
    if(WIN32)
        set(AISDK_MCP_PLATFORM_SOURCE src/mcp/process/WindowsProcess.cpp)
    endif()

    set(AISDK_MCP_SOURCES
        src/mcp/MCPTypes.cpp
        src/mcp/MCPServerConfig.cpp
        src/mcp/MCPProtocol.cpp
        src/mcp/MCPClient.cpp
        src/mcp/MCPToolAdapter.cpp
        src/mcp/StdioMCPTransport.cpp
        src/mcp/StreamableHttpMCPTransport.cpp
        src/mcp/detail/MCPSseDecoder.cpp
        ${AISDK_MCP_PLATFORM_SOURCE}
    )

    # 生产目标与测试专用可注入变体使用同一组源和告警规则，但只允许后者定义测试工厂。
    function(aisdk_configure_mcp_target target_name)
        target_include_directories(${target_name}
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/include
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        target_link_libraries(${target_name}
            PUBLIC
                ai_sdk
                nlohmann_json::nlohmann_json
            PRIVATE
                cpr::cpr
                Threads::Threads
        )

        target_compile_features(${target_name} PUBLIC cxx_std_17)
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /W4 /utf-8)
        else()
            target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endfunction()

    add_library(ai_sdk_mcp ${AISDK_MCP_SOURCES})
    aisdk_configure_mcp_target(ai_sdk_mcp)

    if(AISDK_BUILD_TESTS)
        # HTTP TLS/代理测试只链接该内部静态变体，正式 ai_sdk_mcp 永远不编译测试 CA 与名称解析工厂符号。
        add_library(ai_sdk_mcp_test_support STATIC ${AISDK_MCP_SOURCES})
        aisdk_configure_mcp_target(ai_sdk_mcp_test_support)
        target_compile_definitions(ai_sdk_mcp_test_support PRIVATE AISDK_MCP_TESTING=1)
    endif()
endif()

# 构建图断言在配置期证明依赖只能从 MCP 指向核心，关闭时也不会残留 MCP 源文件。
get_target_property(AISDK_CORE_SOURCES ai_sdk SOURCES)
foreach(AISDK_CORE_SOURCE IN LISTS AISDK_CORE_SOURCES)
    if(AISDK_CORE_SOURCE MATCHES "(^|[/\\])src[/\\]mcp[/\\]")
        message(FATAL_ERROR "核心 ai_sdk 目标不得编译 src/mcp 下的源文件")
    endif()
endforeach()

get_target_property(AISDK_CORE_LINKS ai_sdk LINK_LIBRARIES)
get_target_property(AISDK_CORE_INTERFACE_LINKS ai_sdk INTERFACE_LINK_LIBRARIES)
foreach(AISDK_CORE_LINK IN LISTS AISDK_CORE_LINKS AISDK_CORE_INTERFACE_LINKS)
    if(AISDK_CORE_LINK STREQUAL "ai_sdk_mcp")
        message(FATAL_ERROR "核心 ai_sdk 目标不得链接 ai_sdk_mcp")
    endif()
endforeach()

if(AISDK_BUILD_MCP)
    if(NOT TARGET ai_sdk_mcp)
        message(FATAL_ERROR "AISDK_BUILD_MCP=ON 时必须创建 ai_sdk_mcp 目标")
    endif()
    get_target_property(AISDK_MCP_LINKS ai_sdk_mcp LINK_LIBRARIES)
    if(NOT "ai_sdk" IN_LIST AISDK_MCP_LINKS)
        message(FATAL_ERROR "ai_sdk_mcp 必须单向链接核心 ai_sdk")
    endif()
    get_target_property(AISDK_MCP_DEFINITIONS ai_sdk_mcp COMPILE_DEFINITIONS)
    if("AISDK_MCP_TESTING=1" IN_LIST AISDK_MCP_DEFINITIONS)
        message(FATAL_ERROR "正式 ai_sdk_mcp 目标不得编译 MCP 测试工厂")
    endif()
    if(AISDK_BUILD_TESTS AND NOT TARGET ai_sdk_mcp_test_support)
        message(FATAL_ERROR "启用 MCP 测试时必须创建隔离的测试支持目标")
    endif()
elseif(TARGET ai_sdk_mcp)
    message(FATAL_ERROR "AISDK_BUILD_MCP=OFF 时不得存在 ai_sdk_mcp 目标")
endif()

if(AISDK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(AISDK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
