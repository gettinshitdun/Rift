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
#include <netinet/tcp.h>
#include <poll.h>

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

/**
 * Reliable write helper for browser sockets.
 * Uses poll(POLLOUT) to handle backpressure without blocking the
 * event loop indefinitely. Total timeout ~10 seconds.
 */
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += n;
        } else if (n == 0) {
            return -1;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                int pr = poll(&pfd, 1, 5000);
                if (pr > 0 && !(pfd.revents & (POLLERR | POLLHUP))) continue;
                return -1;
            }
            return -1;
        }
    }
    return 0;
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
        /* Enlarge send buffer for heavy responses */
        int sndbuf = 256 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        connection_add_public(fd);
        metrics_inc_total_connections();
        epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
        printf("[server] public connected fd=%d\n", fd);
    }
}

static void handle_new_tunnel(int epfd, int listener_fd) {
    int fd;
    while ((fd = listener_accept(listener_fd)) > 0) {
        /* Increase socket buffers for heavy content */
        int bufsize = 262144;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

        /* Disable Nagle for lower latency */
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        connection_add_tunnel(fd);
        metrics_inc_total_connections();
        epoll_add_fd(epfd, fd, EPOLLIN | EPOLLHUP | EPOLLERR);
        printf("[server] tunnel connected fd=%d\n", fd);
    }
}

static void handle_initial_frame(int epfd, int fd) {
    char buf[FRAME_MAX_PAYLOAD];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_PEEK);

    if (n < 4) {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        goto fail;
    }

    buf[n] = '\0';

    if (memcmp(buf, "RIFT", 4) == 0) {
        if (handle_rift_frame(fd) == 0) return;
    }
    else if (memcmp(buf, "GET ", 4) == 0 ||
             memcmp(buf, "POST", 4) == 0 ||
             memcmp(buf, "PUT ", 4) == 0 ||
             memcmp(buf, "DELE", 4) == 0 ||
             memcmp(buf, "PATC", 4) == 0 ||
             memcmp(buf, "HEAD", 4) == 0 ||
             memcmp(buf, "OPTI", 4) == 0 ||
             memcmp(buf, "CONN", 4) == 0 ||
             memcmp(buf, "TRAC", 4) == 0) {
        char full_buf[FRAME_MAX_PAYLOAD];
        ssize_t total = recv(fd, full_buf, sizeof(full_buf) - 1, 0);
        if (total > 0) {
            full_buf[total] = '\0';
            int result = handle_http_request(fd, full_buf);
            if (result == 0) return;   /* linked to stream on tunnel */
        }
    }

fail:
    connection_close(epfd, fd);
}

static void handle_connection_event(int epfd, int fd, uint32_t events __attribute__((unused))) {
    connection_t *c = connection_get(fd);
    if (!c) return;

    /* Handle initial handshakes */
    if (c->state == CONN_TUNNEL_INIT || c->state == CONN_PUBLIC_INIT) {
        handle_initial_frame(epfd, fd);
        return;
    }

    char buf[FRAME_MAX_PAYLOAD];
    frame_type_t type;
    uint32_t len;
    uint32_t stream_id;

    switch (c->state) {
        case CONN_PUBLIC_FORWARDING:
            /* Browser -> Server -> Tunnel (as FRAME_DATA tagged with stream_id) */
            {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0) {
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                    if (n == 0) {
                        /* Browser finished sending (HTTP request complete).
                           Don't close — response from tunnel still expected.
                           Just stop reading from this browser fd. */
                        struct epoll_event bev = {
                            .events = EPOLLHUP | EPOLLERR, .data.fd = fd
                        };
                        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &bev);
                        return;
                    }
                    connection_close(epfd, fd);
                    return;
                }
                /* Tag with this browser's stream_id */
                if (frame_write(c->peer_fd, FRAME_DATA, buf, (uint32_t)n, c->stream_id) < 0) {
                    log_error("Failed to forward data to tunnel (fd %d): %s",
                              c->peer_fd, strerror(errno));
                    connection_close(epfd, fd);
                    return;
                }
            }
            break;

        case CONN_TUNNEL_READY:
            /* Tunnel -> Server: read frames and demux by stream_id */
            {
                if (frame_read(fd, &type, buf, &len, &stream_id) != 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                        return;
                    }
                    log_error("Failed to read frame from tunnel fd=%d: %s", fd, strerror(errno));
                    connection_close(epfd, fd);
                    return;
                }

                if (type == FRAME_REGISTER_TUNNEL) {
                    /* Re-registration */
                    snprintf(c->tunnel_id, sizeof(c->tunnel_id), "%.*s", (int)len, buf);
                    log_info("Re-registered tunnel: %s (fd=%d)", c->tunnel_id, fd);
                    return;
                }

                if (type == FRAME_DATA) {
                    /* Route data to the correct browser by stream_id */
                    int browser_fd = tunnel_find_browser(c, stream_id);
                    if (browser_fd < 0) {
                        /* Stream already closed (browser disconnected) — discard */
                        return;
                    }
                    if (write_all(browser_fd, buf, len) < 0) {
                        log_error("Write to browser fd=%d stream=%u failed: %s",
                                  browser_fd, stream_id, strerror(errno));
                        /* Close just this browser, not the tunnel */
                        tunnel_remove_stream(c, stream_id);
                        connection_t *b = connection_get(browser_fd);
                        if (b) {
                            b->peer_fd = 0;
                            b->stream_id = 0;
                        }
                        connection_close(epfd, browser_fd);
                    }
                    return;
                }

                if (type == FRAME_CLOSE) {
                    /* Client's local service closed for this stream — close that browser */
                    int browser_fd = tunnel_find_browser(c, stream_id);
                    tunnel_remove_stream(c, stream_id);
                    if (browser_fd > 0) {
                        connection_t *b = connection_get(browser_fd);
                        if (b) {
                            b->peer_fd = 0;
                            b->stream_id = 0;
                        }
                        connection_close(epfd, browser_fd);
                        fprintf(stderr, "[server] FRAME_CLOSE stream=%u: browser fd=%d closed (tunnel fd=%d, %d active streams)\n",
                                stream_id, browser_fd, fd, c->stream_count);
                    }
                    return;
                }

                if (type == FRAME_ERROR) {
                    log_error("FRAME_ERROR from tunnel fd=%d stream=%u: %.*s",
                              fd, stream_id, (int)len, buf);
                    return;
                }

                log_error("Unexpected frame type %d from tunnel fd=%d", type, fd);
            }
            break;

        default:
            break;
    }
}

/* Event dispatcher */
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
        handle_connection_event(epfd, fd, events);
    }
}

/* Main */
int epoll_server_main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

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
