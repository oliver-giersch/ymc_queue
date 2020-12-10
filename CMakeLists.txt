cmake_minimum_required(VERSION 3.18)
project(ymc_queue)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(lib/wfqueue)

find_package(Threads REQUIRED)

add_library(ymcqueue src/queue.cpp)
target_include_directories(ymcqueue PUBLIC include/ src/)
target_link_libraries(ymcqueue PUBLIC wfqueue)

add_executable(test_single test/test_single.cpp)
target_link_libraries(test_single PUBLIC ymcqueue)
target_compile_options(test_single PRIVATE "-fsanitize=address")
target_link_options(test_single PRIVATE "-fsanitize=address")

add_executable(test_multi test/test_multi.cpp)
target_link_libraries(test_multi PUBLIC ymcqueue)
target_compile_options(test_multi PRIVATE "-fsanitize=address,leak")
target_link_options(test_multi PRIVATE "-fsanitize=address,leak")

target_link_libraries(test_multi PRIVATE
        Threads::Threads
        wfqueue)
