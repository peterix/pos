cmake_minimum_required(VERSION 2.8)
project(unixy_things LANGUAGES C CXX) 

find_package(Threads REQUIRED)

# simple fork example. fork, print some stuff and exit.
add_executable(fork fork.c)

# a pool of threrads sharing some work
add_executable(threadpool threadpool.c)
target_link_libraries(threadpool ${CMAKE_THREAD_LIBS_INIT})

# a simple shell
add_executable(shell shell.cpp)
target_link_libraries(shell ${CMAKE_THREAD_LIBS_INIT})
