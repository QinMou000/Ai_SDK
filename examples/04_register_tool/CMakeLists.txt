add_executable(example_register_tool
    main.cpp
)

target_link_libraries(example_register_tool
    PRIVATE
        ai_sdk
)

if(MSVC)
    target_compile_options(example_register_tool PRIVATE /W4 /utf-8)
else()
    target_compile_options(example_register_tool PRIVATE -Wall -Wextra -Wpedantic)
endif()
