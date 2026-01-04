cmake_minimum_required(VERSION 4.1)
project(rift C)

set(CMAKE_C_STANDARD 11)

add_executable(rift
        server/main.c
        client/main.c
        server/epoll_server.c
        server/include/server.h
        server/include/epoll_loop.h
        server/include/listener.h
        server/include/connection.h
        server/include/forward.h
        server/listner.c
        server/connection.c
        server/forward.c
        server/metrics.c
        server/include/metrics.h
)
