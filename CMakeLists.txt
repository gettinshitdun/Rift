cmake_minimum_required(VERSION 4.1)
project(rift C)

set(CMAKE_C_STANDARD 11)

add_executable(rift
        server/main.c
        client/main.c
)
