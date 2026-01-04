//
// Created by kanishak on 1/4/26.
//

#include "include/listener.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#define BACKLOG 128


int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return -1;

    return 0;
}

/** listener_create(port)
            |
            v
    +------------------+
    | socket()         |
    | AF_INET / TCP    |
    +------------------+
            |
            v
    +------------------+
    | setsockopt()     |
    | SO_REUSEADDR     |
    +------------------+
            |
            v
    +------------------+
    | bind()           |
    | 0.0.0.0:port     |
    +------------------+
            |
            v
    +------------------+
    | listen()         |
    | BACKLOG          |
    +------------------+
            |
            v
    +------------------+
    | fcntl()          |
    | O_NONBLOCK       |
    +------------------+
            |
            v
    +------------------+
    | ready to accept  |
    | connections      |
    +------------------+
**/

int listener_create(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    int opt = 1;
    /*
     *  Make the socket reussable so that in
     *  case of server crash does not have wait 60 seconds.
     *  Mitigate error: address in use
    */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    if (make_nonblocking(fd) < 0) {
        perror("make_nonblocking");
        close(fd);
        return -1;
    }

    return fd;
}

int listener_accept(int listener_fd) {
    while (1) {
        int client_fd = accept(listener_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -1;  // no more clients to accept
            perror("accept");
            return -1;
        }

        if (make_nonblocking(client_fd) < 0) {
            perror("make_nonblocking(client)");
            close(client_fd);
            return -1;
        }

        return client_fd;
    }
}