cmake_minimum_required(VERSION 3.16)
project(test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)

add_executable(test 
    main.cpp
    ../sdk/src/util/Log.cpp
)

target_include_directories(test PRIVATE ../sdk/include)

target_link_libraries(test PRIVATE spdlog::spdlog fmt::fmt)