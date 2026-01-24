#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

#include "include/config.h"
#include "include/listener.h"
#include "include/connection.h"
#include "include/forward.h"
#include "include/metrics.h"
#include "include/frame.h"
#include "include/handlers.h"

#define MAX_EVENTS 64

static volatile sig_atomic_t should_shutdown = 0;

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        should_shutdown = 1;
    }
}

static void log_info(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(stdout, "[%s] [INFO] ", time_buf);
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
    fflush(stdout);
}

static void log_error(const char *fmt, ...) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(stderr, "[%s] [ERROR] ", time_buf);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

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

static void handle_new_public(int epfd, int listener_fd) {
    int fd;
    while ((fd = listener_accept(listener_fd)) > 0) {
        connection_add_public(fd);
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
        connection_add_tunnel(fd);
        connection_t *c = connection_get(fd);
        if (c) c->state = CONN_TUNNEL_INIT;
        metrics_inc_total_connections();
        epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
        printf("[server] tunnel connected fd=%d\n", fd);
    }
}

static void handle_initial_frame(int epfd, int fd) {
    char buf[2048];
    fprintf(stderr, "[DEBUG] handle_initial_frame called for fd=%d\n", fd);
    
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_PEEK);
    fprintf(stderr, "[DEBUG] recv peeked %zd bytes\n", n);

    if (n < 4) {
        fprintf(stderr, "[DEBUG] n < 4, closing connection\n");
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        goto fail;
    }

    buf[n] = '\0';
    fprintf(stderr, "[DEBUG] Peeked data: %.4s\n", buf);

    if (memcmp(buf, "RIFT", 4) == 0) {
        fprintf(stderr, "[DEBUG] Found RIFT header, calling handle_rift_frame\n");
        if (handle_rift_frame(fd) == 0) {
            fprintf(stderr, "[DEBUG] handle_rift_frame returned 0, returning\n");
            return;
        }
        fprintf(stderr, "[DEBUG] handle_rift_frame returned non-zero, going to fail\n");
    }
    else if (memcmp(buf, "GET ", 4) == 0 || memcmp(buf, "POST", 4) == 0) {
        fprintf(stderr, "[DEBUG] Found HTTP header\n");
        char full_buf[2048];
        ssize_t total = recv(fd, full_buf, sizeof(full_buf) - 1, 0);
        if (total > 0) {
            full_buf[total] = '\0';
            if (handle_http_request(fd, full_buf) == 0) return;
        }
    }
    else {
        fprintf(stderr, "[DEBUG] Unknown header: %.4s\n", buf);
    }

fail:
    fprintf(stderr, "[DEBUG] Closing connection due to failure\n");
    connection_close(epfd, fd);
}

static void handle_connection_event(int epfd, int fd) {
    connection_t *c = connection_get(fd);
    if (!c) return;

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
    uint32_t len;

    switch (c->state) {
        case CONN_PUBLIC_FORWARDING:
            /* DIRECTION: Browser -> Server -> Tunnel (as FRAME_DATA) */
            {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                    connection_close(epfd, fd);
                    return;
                }
                // Wrap raw HTTP bytes into a binary RIFT frame
                if (frame_write(c->peer_fd, FRAME_DATA, buf, (uint32_t)n) < 0) {
                    log_error("Failed to forward data to tunnel (fd %d): %s", c->peer_fd, strerror(errno));
                    connection_close(epfd, fd);
                }
            }
            break;

        case CONN_TUNNEL_READY:
            if (frame_read(fd, &type, buf, &len) == 0) {
                if (type == FRAME_DATA) {
                    if (c->peer_fd > 0) {
                        ssize_t sent = write(c->peer_fd, buf, len);
                        if (sent < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                log_error("Write to peer failed: %s", strerror(errno));
                                connection_close(epfd, fd);
                            }
                        } else if (sent != (ssize_t)len) {
                            log_error("Partial write: %ld/%u bytes", sent, len);
                        }
                    }
                } else if (type == FRAME_CLOSE) {
                    log_error("Unexpected FRAME_CLOSE on tunnel in READY state (fd=%d)", fd);
                } else {
                    log_error("Unexpected frame type %d in tunnel mode", type);
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_error("Failed to read frame from tunnel: %s", strerror(errno));
                connection_close(epfd, fd);
            }
            break;

        case CONN_TUNNEL_FORWARDING:
            if (frame_read(fd, &type, buf, &len) == 0) {
                if (type == FRAME_DATA) {
                    ssize_t sent = write(c->peer_fd, buf, len);
                    if (sent < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            log_error("Write to browser failed: %s", strerror(errno));
                            connection_close(epfd, fd);
                        }
                    } else if (sent != (ssize_t)len) {
                        log_error("Partial write to browser: %ld/%u bytes", sent, len);
                    }
                } else if (type == FRAME_CLOSE) {
                    // Local service on client side closed - reset public connection for next request
                    if (c->peer_fd > 0) {
                        connection_reset_public(c->peer_fd);
                    }
                    c->state = CONN_TUNNEL_READY;
                    c->peer_fd = 0;
                } else {
                    log_error("Unexpected frame type %d", type);
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_error("Failed to read frame from tunnel: %s", strerror(errno));
                connection_close(epfd, fd);
            }
            break;

        default:
            break;
    }
}

/* ------------------------- Event dispatcher ------------------------- */

static void dispatch_event(int epfd, int fd, uint32_t events, int t_lsnr, int p_lsnr, int h_lsnr) {
    fprintf(stderr, "[DEBUG] dispatch_event: fd=%d, events=0x%x, t_lsnr=%d, p_lsnr=%d\n", fd, events, t_lsnr, p_lsnr);
    
    if (events & (EPOLLHUP | EPOLLERR)) {
        fprintf(stderr, "[DEBUG] HUP or ERR event, closing\n");
        connection_close(epfd, fd);
        return;
    }

    if (fd == h_lsnr) {
        fprintf(stderr, "[DEBUG] Health listener event\n");
        handle_health_request(fd);
    } else if (fd == t_lsnr) {
        fprintf(stderr, "[DEBUG] Tunnel listener event\n");
        handle_new_tunnel(epfd, fd);
    } else if (fd == p_lsnr) {
        fprintf(stderr, "[DEBUG] Public listener event\n");
        handle_new_public(epfd, fd);
    } else {
        fprintf(stderr, "[DEBUG] Connection event for fd=%d\n", fd);
        handle_connection_event(epfd, fd);
    }
}

/* ------------------------- Main ------------------------- */

int epoll_server_main() {
    // Register signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe

    connection_init();
    metrics_init();

    int tunnel_listener = listener_create(CONFIG_TUNNEL_PORT);
    int public_listener = listener_create(CONFIG_PUBLIC_PORT);
    int health_listener = listener_create(CONFIG_HEALTH_PORT);

    if (tunnel_listener < 0 || public_listener < 0 || health_listener < 0) {
        log_error("Failed to create listeners");
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        log_error("epoll_create1 failed: %s", strerror(errno));
        return 1;
    }

    epoll_add_fd(epfd, tunnel_listener, EPOLLIN);
    epoll_add_fd(epfd, public_listener, EPOLLIN);
    epoll_add_fd(epfd, health_listener, EPOLLIN);

    log_info("RIFT server started");
    log_info("  tunnel listener : %d", CONFIG_TUNNEL_PORT);
    log_info("  public listener : %d", CONFIG_PUBLIC_PORT);
    log_info("  health endpoint : %d", CONFIG_HEALTH_PORT);

    struct epoll_event events[CONFIG_EPOLL_MAX_EVENTS];

    while (!should_shutdown) {
        // Use CONFIG_EPOLL_TIMEOUT_SHUTDOWN during shutdown to allow periodic flag check
        int timeout = should_shutdown ? CONFIG_EPOLL_TIMEOUT_SHUTDOWN : CONFIG_EPOLL_TIMEOUT_NORMAL;
        int nfds = epoll_wait(epfd, events, CONFIG_EPOLL_MAX_EVENTS, timeout);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            log_error("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            dispatch_event(epfd, events[i].data.fd, events[i].events,
                        tunnel_listener, public_listener, health_listener);
        }
    }

    log_info("Shutting down gracefully...");
    close(tunnel_listener);
    close(public_listener);
    close(health_listener);
    close(epfd);
    log_info("RIFT server stopped");

    return 0;
}