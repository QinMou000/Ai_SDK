cmake_minimum_required(VERSION 3.16)
project(AIChatServer LANGUAGES CXX)

# ==============================================================================
# 1. 基础设置
# ==============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# ==============================================================================
# 2. 查找依赖库
# ==============================================================================
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(GFlags REQUIRED)
find_package(OpenSSL REQUIRED)

# 查找httplib (header-only库)
find_path(HTTPLIB_INCLUDE_DIR NAMES httplib.h)
if(HTTPLIB_INCLUDE_DIR)
    message(STATUS "Found httplib: ${HTTPLIB_INCLUDE_DIR}")
else()
    message(WARNING "httplib not found, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.18.0
    )
    FetchContent_MakeAvailable(httplib)
    set(HTTPLIB_INCLUDE_DIR ${httplib_SOURCE_DIR})
endif()

# 查找JsonCpp
find_package(jsoncpp QUIET)
if(NOT jsoncpp_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JSONCPP REQUIRED jsoncpp)
    set(JSONCPP_LIB_TARGET ${JSONCPP_LIBRARIES})
else()
    if(TARGET JsonCpp::JsonCpp)
        set(JSONCPP_LIB_TARGET JsonCpp::JsonCpp)
    elseif(TARGET jsoncpp_lib)
        set(JSONCPP_LIB_TARGET jsoncpp_lib)
    else()
        set(JSONCPP_LIB_TARGET jsoncpp)
    endif()
endif()

# ==============================================================================
# 3. 定义源文件
# ==============================================================================
set(CHATSERVER_SOURCES
    ChatServer.cpp
    main.cpp
    ../sdk/src/DeepSeekProvider.cpp
    ../sdk/src/ChatGPTProvider.cpp
    ../sdk/src/GeminiProvider.cpp
    ../sdk/src/LocalLLMProvider.cpp
    ../sdk/src/LLMManager.cpp
    ../sdk/src/SessionManager.cpp
    ../sdk/src/DataManager.cpp
    ../sdk/src/util/Log.cpp
)

# ==============================================================================
# 4. 目标配置
# ==============================================================================
add_executable(AIChatServer ${CHATSERVER_SOURCES})

# 启用httplib的HTTPS支持（需要OpenSSL）
target_compile_definitions(AIChatServer PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# Windows特定编译定义
if(WIN32)
    target_compile_definitions(AIChatServer PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0601
    )
endif()

target_include_directories(AIChatServer PRIVATE
    .
    ../sdk/include
    ${OPENSSL_INCLUDE_DIR}
    ${JSONCPP_INCLUDE_DIRS}
    ${GFLAGS_INCLUDE_DIR}
    ${HTTPLIB_INCLUDE_DIR}
)

# 跨平台线程库处理
if(WIN32)
    # Windows上使用Threads库（包含在MSVC运行时中）
    set(THREADS_LIBRARY Threads::Threads)
    # Windows上gflags需要静态链接
    if(TARGET gflags_static)
        set(GFLAGS_LIBRARY gflags_static)
    else()
        set(GFLAGS_LIBRARY GFlags::gflags)
    endif()
else()
    # Linux上使用pthread
    set(THREADS_LIBRARY pthread)
    set(GFLAGS_LIBRARY GFlags::gflags)
endif()

target_link_libraries(AIChatServer PRIVATE
    spdlog::spdlog
    fmt::fmt
    ${GFLAGS_LIBRARY}
    OpenSSL::SSL
    OpenSSL::Crypto
    sqlite3
    ${JSONCPP_LIB_TARGET}
    ${THREADS_LIBRARY}
)

# 复制前端资源到构建目录
add_custom_command(TARGET AIChatServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/web
    $<TARGET_FILE_DIR:AIChatServer>/web
)

# 复制配置文件模板
configure_file(
    ${CMAKE_SOURCE_DIR}/ChatServer.conf.in
    ${CMAKE_BINARY_DIR}/ChatServer.conf
    @ONLY
)

add_custom_command(TARGET AIChatServer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/ChatServer.conf
    $<TARGET_FILE_DIR:AIChatServer>/ChatServer.conf
)
