//
// Created by kanishak on 1/4/26.
//

#include "include/connection.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>


static connection_t connections[MAX_CONNECTIONS];

// Initialize connection table
void connection_init(void) {
    memset(connections, 0, sizeof(connections));
}

// Add a new fd
void connection_add(int fd) {
    if (fd <= 0 || fd >= MAX_CONNECTIONS) {
        fprintf(stderr, "[connection] fd %d out of range\n", fd);
        return;
    }
    connections[fd].fd = fd;
    connections[fd].peer_fd = 0;
}

// Pair two fds
void connection_pair(int fd1, int fd2) {
    if (fd1 <= 0 || fd1 >= MAX_CONNECTIONS || fd2 <= 0 || fd2 >= MAX_CONNECTIONS) {
        fprintf(stderr, "[connection] fd out of range for pairing\n");
        return;
    }
    connections[fd1].peer_fd = fd2;
    connections[fd2].peer_fd = fd1;
}

// Get peer fd
int connection_get_peer(int fd) {
    if (fd <= 0 || fd >= MAX_CONNECTIONS) return -1;
    return connections[fd].peer_fd;
}

// Close connection and peer
void connection_close(int epfd, int fd) {
    if (fd <= 0 || fd >= MAX_CONNECTIONS) return;

    int peer = connections[fd].peer_fd;

    // Remove from epoll
    if (epfd >= 0) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
        if (peer > 0)
            epoll_ctl(epfd, EPOLL_CTL_DEL, peer, NULL);
    }

    // Close sockets
    if (fd > 0) close(fd);
    if (peer > 0) close(peer);

    // Clear entries
    connections[fd].fd = 0;
    connections[fd].peer_fd = 0;

    if (peer > 0 && peer < MAX_CONNECTIONS) {
        connections[peer].fd = 0;
        connections[peer].peer_fd = 0;
    }
}
