add_executable(example_simple_agent
    main.cpp
)

target_link_libraries(example_simple_agent
    PRIVATE
        ai_sdk
)

if(MSVC)
    target_compile_options(example_simple_agent PRIVATE /W4 /utf-8)
else()
    target_compile_options(example_simple_agent PRIVATE -Wall -Wextra -Wpedantic)
endif()
