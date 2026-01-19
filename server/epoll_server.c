#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <string.h>

#include "include/listener.h"
#include "include/connection.h"
#include "include/forward.h"
#include "include/metrics.h"
#include "include/frame.h"

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

        if (write(client_fd, header, header_len) < 0) perror("write header");
        if (write(client_fd, body, body_len) < 0) perror("write body");

        close(client_fd);
    }
}

/* ------------------------- Listener handlers ------------------------- */

static void handle_new_public(int epfd, int listener_fd) {
    int fd;
    while ((fd = listener_accept(listener_fd)) > 0) {
        connection_add_public(fd);
        metrics_inc_total_connections();
        epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
        printf("[server] public connected fd=%d\n", fd);
    }
}

static void handle_new_tunnel(int epfd, int listener_fd) {
    int fd;
    while ((fd = listener_accept(listener_fd)) > 0) {
        connection_add_tunnel(fd);
        metrics_inc_total_connections();
        epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
        printf("[server] tunnel connected fd=%d\n", fd);
    }
}

/* ------------------------- Connection handlers ------------------------- */

static void handle_initial_frame(int epfd, int fd) {
    frame_type_t type;
    char payload[FRAME_MAX_PAYLOAD + 1];
    uint16_t len;

    if (frame_read(fd, &type, payload, &len) < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        connection_close(epfd, fd);
        return;
    }

    connection_t *c = connection_get(fd);
    if (!c) {
        connection_close(epfd, fd);
        return;
    }

    if (type == FRAME_REGISTER_TUNNEL) {
        size_t copy_len = len < sizeof(c->tunnel_id) - 1 ? len : sizeof(c->tunnel_id) - 1;
        memcpy(c->tunnel_id, payload, copy_len);
        c->tunnel_id[copy_len] = '\0';

        c->state = CONN_TUNNEL_READY;
        printf("[tunnel] registered id=%s fd=%d\n", c->tunnel_id, fd);
        return;
    }

    if (type == FRAME_CONNECT_REQUEST) {
        size_t copy_len = len < sizeof(c->service_id) - 1 ? len : sizeof(c->service_id) - 1;
        memcpy(c->service_id, payload, copy_len);
        c->service_id[copy_len] = '\0';

        connection_t *tunnel = connection_find_tunnel(c->service_id);
        if (!tunnel) {
            printf("[public] no tunnel for %s\n", c->service_id);
            connection_close(epfd, fd);
            return;
        }

        connection_bind(fd, tunnel->fd);
        printf("[bind] public fd=%d -> tunnel fd=%d (%s)\n", fd, tunnel->fd, c->service_id);
        return;
    }

    connection_close(epfd, fd);
}

static void handle_connection_event(int epfd, int fd) {
    connection_t *c = connection_get(fd);
    if (!c) {
        connection_close(epfd, fd);
        return;
    }

    if (c->state == CONN_TUNNEL_INIT || c->state == CONN_PUBLIC_INIT) {
        handle_initial_frame(epfd, fd);
        return;
    }

    if (c->peer_fd > 0) {
        forward_data(fd, c->peer_fd);
    }
}

/* ------------------------- Event dispatcher ------------------------- */

static void dispatch_event(int epfd, int fd, uint32_t events, int t_lsnr, int p_lsnr, int h_lsnr) {
    if (events & (EPOLLHUP | EPOLLERR)) {
        connection_close(epfd, fd);
        return;
    }

    if (fd == h_lsnr) {
        handle_health_request(fd);
    } else if (fd == t_lsnr) {
        handle_new_tunnel(epfd, fd);
    } else if (fd == p_lsnr) {
        handle_new_public(epfd, fd);
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

    printf("[server] running\n  tunnel : 7000\n  public : 9000\n  health : 8080\n");

    struct epoll_event events[MAX_EVENTS];

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            exit(1);
        }

        for (int i = 0; i < nfds; i++) {
            dispatch_event(epfd, events[i].data.fd, events[i].events, 
                           tunnel_listener, public_listener, health_listener);
        }
    }
}