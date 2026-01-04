#include "include/connection.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/epoll.h>

static connection_t connections[MAX_CONNECTIONS];
static int active_connections = 0;

void connection_init(void) {
    memset(connections, 0, sizeof(connections));
}

void connection_add(int fd, int type) {
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].fd == 0) {
            connections[i].fd = fd;
            connections[i].type = type;
            connections[i].peer_fd = 0;
            active_connections++;

            // Try to pair
            int peer_fd = connection_get_unpaired(type ^ 1); // opposite type
            if (peer_fd > 0) {
                connections[i].peer_fd = peer_fd;
                connection_set_peer(peer_fd, fd);
                printf("[connection] paired fd %d <-> %d\n", fd, peer_fd);
            }
            return;
        }
    }
    printf("[connection] warning: connection table full for fd=%d\n", fd);
}

int connection_get_peer(int fd) {
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].fd == fd)
            return connections[i].peer_fd;
    }
    return 0;
}

void connection_set_peer(int fd, int peer_fd) {
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].fd == fd) {
            connections[i].peer_fd = peer_fd;
            return;
        }
    }
}

int connection_get_unpaired(int type) {
    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].fd > 0 && connections[i].type == type && connections[i].peer_fd == 0)
            return connections[i].fd;
    }
    return 0;
}

void connection_close(int epfd, int fd) {
    int peer = connection_get_peer(fd);

    // First, remove the current fd
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    for (int i = 0; i < MAX_CONNECTIONS; ++i) {
        if (connections[i].fd == fd) {
            connections[i].fd = 0;
            connections[i].peer_fd = 0;
            connections[i].type = 0;
            active_connections--;
        }
    }
    printf("[connection] closed fd=%d\n", fd);

    // If there is a peer, close it too
    if (peer > 0) {
        printf("[connection] closing peer fd=%d\n", peer);
        connection_close(epfd, peer); // recursive call
    }
}

int connection_active_count() {
    return active_connections;
}