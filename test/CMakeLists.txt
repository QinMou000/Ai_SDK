cmake_minimum_required(VERSION 3.16)
project(test LANGUAGES CXX)

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
find_package(GTest REQUIRED)
find_package(OpenSSL REQUIRED)

# 查找 JsonCpp (优先尝试 Config 模式，失败则回退到 PkgConfig)
find_package(jsoncpp QUIET)
if(NOT jsoncpp_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JSONCPP REQUIRED jsoncpp)
    set(JSONCPP_LIB_TARGET ${JSONCPP_LIBRARIES})
else()
    # 不同的发行版 target 名字可能不同，通常是 JsonCpp::JsonCpp 或 jsoncpp_lib
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
set(SDK_SOURCES
    ../sdk/src/DeepSeekProvider.cpp
    ../sdk/src/ChatGPTProvider.cpp
    ../sdk/src/GeminiProvider.cpp
    ../sdk/src/LocalLLMProvider.cpp
    ../sdk/src/LLMManager.cpp
    ../sdk/src/util/Log.cpp
)

# ==============================================================================
# 4. 目标配置
# ==============================================================================
add_executable(test 
    main.cpp
    ${SDK_SOURCES}
)

# 启用 httplib 的 HTTPS 支持 (需要 OpenSSL)
target_compile_definitions(test PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_include_directories(test PRIVATE 
    ../sdk/include
    ${OPENSSL_INCLUDE_DIR}
    ${JSONCPP_INCLUDE_DIRS}
)

target_link_libraries(test PRIVATE 
    spdlog::spdlog
    fmt::fmt
    GTest::gtest
    OpenSSL::SSL
    OpenSSL::Crypto
    sqlite3
    ${JSONCPP_LIB_TARGET}
    pthread
)
