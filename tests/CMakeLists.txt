find_package(GTest CONFIG REQUIRED)

add_executable(ai_sdk_smoke_test
    smoke/ai_sdk_smoke_test.cpp
)

target_link_libraries(ai_sdk_smoke_test
    PRIVATE
        ai_sdk
        GTest::gtest_main
)

if(MSVC)
    target_compile_options(ai_sdk_smoke_test PRIVATE /W4 /utf-8)
else()
    target_compile_options(ai_sdk_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_subdirectory(core)
add_subdirectory(provider)
add_subdirectory(tool)
add_subdirectory(http)

add_test(NAME ai_sdk_smoke_test COMMAND ai_sdk_smoke_test)