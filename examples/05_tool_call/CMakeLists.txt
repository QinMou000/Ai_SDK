add_executable(example_tool_call
    main.cpp
)

target_link_libraries(example_tool_call
    PRIVATE
        ai_sdk
)

if(MSVC)
    target_compile_options(example_tool_call PRIVATE /W4 /utf-8)
else()
    target_compile_options(example_tool_call PRIVATE -Wall -Wextra -Wpedantic)
endif()
