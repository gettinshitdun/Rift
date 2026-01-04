//
// Created by kanishak on 1/4/26.
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>

#include "include/listener.h"
#include "include/connection.h"
#include "include/forward.h"

#define MAX_EVENTS 64

void handle_new_clients(int epfd, int listener_fd, int type, const char *type_str) {
    struct epoll_event ev;
    int client_fd;
    while ((client_fd = listener_accept(listener_fd)) > 0) {
        connection_add(client_fd, type);  // pass type here
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
        printf("[server] %s client connected: fd=%d\n", type_str, client_fd);
    }
}

// Forward data for existing connections or close if peer missing
void handle_existing_connection(int epfd, int fd) {
    int peer = connection_get_peer(fd);
    if (peer > 0) {
        forward_data(fd, peer);
    } else {
        connection_close(epfd, fd);
    }
}

int epoll_server_main() {
    // Initialize connection table
    connection_init();

    // Create listener sockets
    int tunnel_listener = listener_create(7000);
    int public_listener = listener_create(9000);

    // Create epoll instance
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        exit(1);
    }

    struct epoll_event ev, events[MAX_EVENTS];

    // Add listeners to epoll
    ev.events = EPOLLIN;
    ev.data.fd = tunnel_listener;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, tunnel_listener, &ev) < 0) {
        perror("epoll_ctl add tunnel_listener");
        exit(1);
    }

    ev.data.fd = public_listener;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, public_listener, &ev) < 0) {
        perror("epoll_ctl add public_listener");
        exit(1);
    }

    printf("[server] epoll server running. Tunnel:7000, Public:9000\n");

    // Event loop
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            exit(1);
        }

        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                // Peer disconnected or socket error
                printf("[server] fd %d disconnected\n", fd);
                connection_close(epfd, fd);
                continue;
            }
            if (fd == tunnel_listener) {
                handle_new_clients(epfd, tunnel_listener,0, "tunnel");
            } else if (fd == public_listener) {
                handle_new_clients(epfd, public_listener,1, "public");
            } else {
                handle_existing_connection(epfd, fd);
            }
        }
    }

    close(tunnel_listener);
    close(public_listener);
    close(epfd);

    return 0;
}
