#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>

#include "include/listener.h"
#include "include/connection.h"
#include "include/forward.h"
#include "include/metrics.h"
#include "include/frame.h"
#include "include/handlers.h" 

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
        // 1. Add the connection to the internal table
        connection_add_public(fd);
        
        // 2. Fetch the pointer to the connection we just created
        connection_t *c = connection_get(fd);
        if (c) c->state = CONN_PUBLIC_INIT; 
        
        metrics_inc_total_connections();
        epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
        printf("[server] public connected fd=%d\n", fd);
    }
}

static void handle_new_tunnel(int epfd, int listener_fd) {
    int fd;
    while ((fd = listener_accept(listener_fd)) > 0) {
        // 1. Add the connection to the internal table
        connection_add_tunnel(fd);
        
        // 2. Fetch the pointer to the connection we just created
        connection_t *c = connection_get(fd);
        if (c) c->state = CONN_TUNNEL_INIT;

        metrics_inc_total_connections();
        epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
        printf("[server] tunnel connected fd=%d\n", fd);
    }
}

/* ------------------------- Connection handlers ------------------------- */

static void handle_initial_frame(int epfd, int fd) {
    char peek_buf[4];
    // Peek just enough to distinguish the protocol
    ssize_t n = recv(fd, peek_buf, 4, MSG_PEEK);
    
    if (n < 4) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        goto fail;
    }

    // 1. Identify RIFT Traffic (CLI Client)
    if (memcmp(peek_buf, "RIFT", 4) == 0) {
        if (handle_rift_frame(fd) == 0) return;
    } 
    // 2. Identify HTTP Traffic (Browser)
    else if (memcmp(peek_buf, "GET ", 4) == 0 || memcmp(peek_buf, "POST", 4) == 0) {
        char full_buf[2048];
        ssize_t total = recv(fd, full_buf, sizeof(full_buf) - 1, 0); // Consume for parsing
        if (total > 0) {
            full_buf[total] = '\0';
            if (handle_http_request(fd, full_buf) == 0) return;
        }
    }

fail:
    connection_close(epfd, fd);
}

static void handle_connection_event(int epfd, int fd) {
    connection_t *c = connection_get(fd);
    if (!c) return;

    // Trace every event
    printf("[debug] Event on fd %d, State: %d, Peer: %d\n", fd, c->state, c->peer_fd);

    // Handle initial handshakes first
    if (c->state == CONN_TUNNEL_INIT || c->state == CONN_PUBLIC_INIT) {
        handle_initial_frame(epfd, fd);
        return;
    }

    // If we are in forwarding mode but peer is missing, something went wrong
    if (c->peer_fd <= 0) {
        connection_close(epfd, fd);
        return;
    }

    char buf[FRAME_MAX_PAYLOAD];
    frame_type_t type;
    uint16_t len;

    switch (c->state) {
        case CONN_PUBLIC_FORWARDING:
            /* DIRECTION: Browser -> Server -> Tunnel */
            {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                    connection_close(epfd, fd);
                    return;
                }
                // Wrap raw HTTP bytes into a binary RIFT frame
                if (frame_write(c->peer_fd, FRAME_DATA, buf, (uint16_t)n) < 0) {
                    connection_close(epfd, fd);
                }
            }
            break;

        case CONN_TUNNEL_READY:
            /* DIRECTION: Tunnel Client -> Server -> Browser */
            if (frame_read(fd, &type, buf, &len) == 0) {
                if (type == FRAME_DATA) {
                    printf("[debug] Frame received from tunnel. Type: DATA, Len: %d\n", len);
                    ssize_t sent = write(c->peer_fd, buf, len);
                    if (sent > 0) {
                        printf("[debug] Successfully wrote %ld bytes back to browser (fd: %d)\n", sent, c->peer_fd);
                    } else {
                        perror("[!] Write to browser failed");
                    }
                }
            }

        default:
            break;
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

    printf("[server] RIFT active\n  tunnel : 7000\n  public : 9000\n  health : 8080\n");

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
    return 0;
}