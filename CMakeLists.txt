cmake_minimum_required(VERSION 3.18)
project(ymc_queue)

set(CMAKE_CXX_STANDARD 20)

add_library(ymc_queue src/queue.cpp)
target_include_directories(ymc_queue PUBLIC include/ src/)

add_executable(test_single test/test_single.cpp)
target_link_libraries(test_single PUBLIC ymc_queue)
target_compile_options(test_single PRIVATE "-fsanitize=address")
target_link_options(test_single PRIVATE "-fsanitize=address")
