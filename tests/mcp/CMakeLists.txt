add_executable(ai_sdk_mcp_test
    mcp_types_config_test.cpp
    mcp_protocol_test.cpp
    mcp_sse_decoder_test.cpp
    mcp_client_test.cpp
    mcp_adapter_test.cpp
)

target_include_directories(ai_sdk_mcp_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(ai_sdk_mcp_test
    PRIVATE
        ai_sdk_mcp
        GTest::gtest_main
)

add_executable(mcp_stdio_fixture
    helpers/mcp_stdio_fixture.cpp
)

target_link_libraries(mcp_stdio_fixture
    PRIVATE
        nlohmann_json::nlohmann_json
)

add_executable(ai_sdk_mcp_stdio_test
    mcp_stdio_integration_test.cpp
)

# CTest 的 $<TARGET_FILE:mcp_stdio_fixture> 只提供运行期路径，不会建立显式构建依赖。
# 保证单独构建 stdio 集成测试时夹具也会随源码同步重编译，避免旧二进制掩盖新增模式。
add_dependencies(ai_sdk_mcp_stdio_test mcp_stdio_fixture)

target_link_libraries(ai_sdk_mcp_stdio_test
    PRIVATE
        ai_sdk_mcp
        GTest::gtest
)

add_executable(ai_sdk_mcp_http_test
    mcp_http_integration_test.cpp
)

target_include_directories(ai_sdk_mcp_http_test
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)

set(AISDK_MCP_TLS_TEST_SERVER_SOURCE helpers/PosixMcpTlsTestServer.cpp)
if(WIN32)
    set(AISDK_MCP_TLS_TEST_SERVER_SOURCE helpers/WindowsMcpTlsTestServer.cpp)
else()
    # Linux TLS 夹具复用 cpr SSL 依赖图中的 OpenSSL，不增加产品依赖。
    find_package(OpenSSL REQUIRED)
endif()

add_library(ai_sdk_mcp_tls_test_server STATIC
    ${AISDK_MCP_TLS_TEST_SERVER_SOURCE}
)

target_include_directories(ai_sdk_mcp_tls_test_server
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers
)

target_link_libraries(ai_sdk_mcp_tls_test_server
    PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
)

if(WIN32)
    target_link_libraries(ai_sdk_mcp_tls_test_server PRIVATE ws2_32 secur32 crypt32)
else()
    target_link_libraries(ai_sdk_mcp_tls_test_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

target_link_libraries(ai_sdk_mcp_http_test
    PRIVATE
        ai_sdk_mcp_test_support
        ai_sdk_mcp_tls_test_server
        GTest::gtest_main
        Threads::Threads
)

if(WIN32)
    target_link_libraries(ai_sdk_mcp_http_test PRIVATE ws2_32)
endif()

foreach(AISDK_MCP_TEST_TARGET IN ITEMS ai_sdk_mcp_test mcp_stdio_fixture ai_sdk_mcp_stdio_test ai_sdk_mcp_http_test ai_sdk_mcp_tls_test_server)
    if(MSVC)
        target_compile_options(${AISDK_MCP_TEST_TARGET} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${AISDK_MCP_TEST_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

add_test(NAME ai_sdk_mcp_test COMMAND ai_sdk_mcp_test)
add_test(NAME ai_sdk_mcp_stdio_test COMMAND ai_sdk_mcp_stdio_test $<TARGET_FILE:mcp_stdio_fixture>)
add_test(NAME ai_sdk_mcp_http_test COMMAND ai_sdk_mcp_http_test)

set_tests_properties(ai_sdk_mcp_test PROPERTIES LABELS "mcp" TIMEOUT 30)
set_tests_properties(ai_sdk_mcp_stdio_test PROPERTIES LABELS "mcp;integration;stdio" TIMEOUT 30)
set_tests_properties(ai_sdk_mcp_http_test PROPERTIES LABELS "mcp;integration;http" TIMEOUT 30 RUN_SERIAL TRUE)
