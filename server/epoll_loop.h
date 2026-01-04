//
// Created by kanishak on 1/4/26.
//

#ifndef RIFT_EPOLL_LOOP_H
#define RIFT_EPOLL_LOOP_H
// server/epoll_loop.h
#pragma once

// Initialize epoll instance
int epoll_loop_init(void);

// Add fd to epoll (EPOLLIN | EPOLLET)
int epoll_loop_add(int epfd, int fd);

// Remove fd from epoll
int epoll_loop_remove(int epfd, int fd);

// Run event loop (blocks)
void epoll_loop_run(int epfd);


#endif //RIFT_EPOLL_LOOP_H