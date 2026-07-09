add_executable(example_stream_chat
    main.cpp
)

target_link_libraries(example_stream_chat
    PRIVATE
        ai_sdk
)

if(MSVC)
    target_compile_options(example_stream_chat PRIVATE /W4 /utf-8)
else()
    target_compile_options(example_stream_chat PRIVATE -Wall -Wextra -Wpedantic)
endif()
