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
#include "include/metrics.h"

#define MAX_EVENTS 64

/* ------------------------- Epoll helpers ------------------------- */

static void epoll_add_fd(int epfd, int fd, uint32_t events) {
    struct epoll_event ev = {
        .events = events,
        .data.fd = fd
    };

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        perror("epoll_ctl ADD");
        close(fd);
    }
}

/* ------------------------- Health handler ------------------------- */

void handle_health_request(int listener_fd) {
    int client_fd;
    while ((client_fd = listener_accept(listener_fd)) > 0) {

        char body[512];
        int body_len = snprintf(body, sizeof(body),
            "status: OK\n"
            "uptime_sec: %ld\n"
            "active_connections: %d\n"
            "total_connections: %d\n",
            metrics_uptime(),
            connection_active_count(),
            metrics_total_connections()
        );

        char header[256];
        int header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n",
            body_len
        );

        /* write header */
        ssize_t w = write(client_fd, header, header_len);
        (void)w; // silence -Wunused-result if you prefer

        /* write body */
        w = write(client_fd, body, body_len);
        (void)w;

        close(client_fd);
    }
}

/* ------------------------- Listener handlers ------------------------- */

static void handle_new_clients(
    int epfd,
    int listener_fd,
    int conn_type,
    const char *label
) {
    int client_fd;

    while ((client_fd = listener_accept(listener_fd)) > 0) {
        connection_add(client_fd, conn_type);
        metrics_inc_total_connections();

        epoll_add_fd(epfd, client_fd, EPOLLIN | EPOLLET | EPOLLHUP | EPOLLERR);
        printf("[server] %s client connected: fd=%d\n", label, client_fd);
    }
}

/* ------------------------- Connection handler ------------------------- */

static void handle_connection_event(int epfd, int fd) {
    int peer = connection_get_peer(fd);

    if (peer > 0) {
        forward_data(fd, peer);
    } else {
        connection_close(epfd, fd);
    }
}

/* ------------------------- Event dispatcher ------------------------- */

static void dispatch_event(
    int epfd,
    int fd,
    uint32_t events,
    int tunnel_listener,
    int public_listener,
    int health_listener
) {
    if (events & (EPOLLHUP | EPOLLERR)) {
        printf("[server] fd %d disconnected\n", fd);
        connection_close(epfd, fd);
        return;
    }

    if (fd == health_listener) {
        handle_health_request(health_listener);
    } else if (fd == tunnel_listener) {
        handle_new_clients(epfd, tunnel_listener, 0, "tunnel");
    } else if (fd == public_listener) {
        handle_new_clients(epfd, public_listener, 1, "public");
    } else {
        handle_connection_event(epfd, fd);
    }
}

/* ------------------------- Main ------------------------- */

int epoll_server_main() {
    connection_init();
    metrics_init();

    int tunnel_listener = listener_create(7000);
    int public_listener = listener_create(9000);
    int health_listener = listener_create(8080);

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        exit(1);
    }

    epoll_add_fd(epfd, tunnel_listener, EPOLLIN);
    epoll_add_fd(epfd, public_listener, EPOLLIN);
    epoll_add_fd(epfd, health_listener, EPOLLIN);

    printf("[server] running\n");
    printf("  tunnel : 7000\n");
    printf("  public : 9000\n");
    printf("  health : 8080\n");

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            exit(1);
        }

        for (int i = 0; i < nfds; i++) {
            dispatch_event(
                epfd,
                events[i].data.fd,
                events[i].events,
                tunnel_listener,
                public_listener,
                health_listener
            );
        }
    }
}
