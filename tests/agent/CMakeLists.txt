add_executable(ai_sdk_agent_test
    simple_agent_test.cpp
    workspace_file_tools_test.cpp
)

target_link_libraries(ai_sdk_agent_test
    PRIVATE
        ai_sdk
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(ai_sdk_agent_test PRIVATE /W4 /utf-8)
else()
    target_compile_options(ai_sdk_agent_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME ai_sdk_agent_test COMMAND ai_sdk_agent_test)
