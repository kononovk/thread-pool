cmake_minimum_required(VERSION 2.8)

project(thread-pool)

set(CMAKE_CXX_STANDARD 20)


set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror -Wpessimizing-move -Wextra-semi -Wno-self-assign-overloaded
-O1 -g -fsanitize=address -fno-omit-frame-pointer -pthread")

set(CMAKE_CXX_FLAGS "-fsanitize=address")

add_executable(thread-pool main.cpp thread_pool.hpp)
