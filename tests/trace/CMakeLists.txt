add_executable(ai_sdk_trace_test
    trace_recorder_test.cpp
    trace_integration_test.cpp
)

target_link_libraries(ai_sdk_trace_test
    PRIVATE
        ai_sdk
        GTest::gtest_main
        Threads::Threads
)

# Trace 测试与主库使用相同告警级别，确保并发和类型转换问题不会被测试目标掩盖。
if(MSVC)
    target_compile_options(ai_sdk_trace_test PRIVATE /W4 /utf-8)
else()
    target_compile_options(ai_sdk_trace_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME ai_sdk_trace_test COMMAND ai_sdk_trace_test)
