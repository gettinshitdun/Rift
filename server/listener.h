//
// Created by kanishak on 1/4/26.
//

#ifndef RIFT_LISTENER_H
#define RIFT_LISTENER_H

// Create a TCP listening socket on given port
// returns fd or -1 on error
int listener_create(int port);

// Accept a new client from listener fd
// returns client fd or -1 if none available
int listener_accept(int listener_fd);

// Make fd non-blocking
int make_nonblocking(int fd);

#endif //RIFT_LISTENER_H