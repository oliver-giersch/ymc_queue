cmake_minimum_required(VERSION 3.16)
project(wfqueue C)

set(CMAKE_C_STANDARD 90)

add_library(wfqueue STATIC wfqueue.c)
target_include_directories(wfqueue PUBLIC .)
