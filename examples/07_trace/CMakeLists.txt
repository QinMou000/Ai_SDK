add_executable(example_trace
    main.cpp
)

target_link_libraries(example_trace
    PRIVATE
        ai_sdk
)

# 示例与主库保持相同告警级别和 UTF-8 编码设置。
if(MSVC)
    target_compile_options(example_trace PRIVATE /W4 /utf-8)
else()
    target_compile_options(example_trace PRIVATE -Wall -Wextra -Wpedantic)
endif()
