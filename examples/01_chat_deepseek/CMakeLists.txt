add_executable(example_chat_deepseek
    main.cpp
)

target_link_libraries(example_chat_deepseek
    PRIVATE
        ai_sdk
)

if(MSVC)
    target_compile_options(example_chat_deepseek PRIVATE /W4 /utf-8)
else()
    target_compile_options(example_chat_deepseek PRIVATE -Wall -Wextra -Wpedantic)
endif()
