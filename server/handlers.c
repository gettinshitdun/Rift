#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>
#include "include/handlers.h"
#include "include/connection.h"
#include "include/frame.h"

void send_http_error(int fd, const char *status, const char *msg) {
    char resp[256];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", status, strlen(msg), msg);
    if (write(fd, resp, len) < 0) perror("write error");
}

int handle_http_request(int fd, const char *peek_buf) {
    char tunnel_id[64] = {0};

    /* Try X-Tunnel-Id header first */
    char *custom_hdr = strstr(peek_buf, "x-tunnel-id: ");
    if (!custom_hdr)
        custom_hdr = strstr(peek_buf, "X-Tunnel-Id: ");
    if (custom_hdr) {
        const char *start = strchr(custom_hdr, ':') + 2;
        sscanf(start, "%63s", tunnel_id);
    } else {
        /* Fall back to Host header (subdomain extraction) */
        char *host_hdr = strstr(peek_buf, "Host: ");
        if (host_hdr)
            sscanf(host_hdr + 6, "%63[^.:\r\n]", tunnel_id);
    }

    if (tunnel_id[0] != '\0') {
        connection_t *tunnel = connection_find_tunnel(tunnel_id);
        if (tunnel) {
            connection_bind(fd, tunnel->fd);

            if (frame_write(tunnel->fd, FRAME_CONNECT_REQUEST, "NEW", 3) < 0) {
                fprintf(stderr, "[http] Failed to write FRAME_CONNECT_REQUEST: %s\n", strerror(errno));
                return -1;
            }

            size_t hdr_len = strlen(peek_buf);
            if (hdr_len > FRAME_MAX_PAYLOAD) {
                fprintf(stderr, "[http] HTTP request too large: %zu bytes (max %u)\n", hdr_len, FRAME_MAX_PAYLOAD);
                return -1;
            }

            if (frame_write(tunnel->fd, FRAME_DATA, peek_buf, (uint32_t)hdr_len) < 0) {
                fprintf(stderr, "[http] Failed to forward initial request to tunnel: %s\n", strerror(errno));
                return -1;
            }

            fprintf(stderr, "[http] Linked Browser(fd=%d) to Tunnel(%s), sent %zu bytes\n",
                    fd, tunnel_id, hdr_len);
            return 0;
        }

        /* Tunnel exists but is busy — queue the request */
        connection_t *busy = connection_find_tunnel_any(tunnel_id);
        if (busy) {
            size_t hdr_len = strlen(peek_buf);
            if (pending_queue_push(tunnel_id, fd, peek_buf, hdr_len) == 0) {
                fprintf(stderr, "[http] Tunnel %s busy, queued Browser(fd=%d)\n",
                        tunnel_id, fd);
                return 1;  /* 1 = queued, caller should NOT close */
            }
            fprintf(stderr, "[http] Pending queue full for %s\n", tunnel_id);
        } else {
            fprintf(stderr, "[http] Tunnel not found: %s\n", tunnel_id);
        }
    } else {
        fprintf(stderr, "[http] No tunnel ID in request\n");
    }

    send_http_error(fd, "404 Not Found", "No Tunnel Specified or Found\n");
    return -1;
}

void serve_next_pending(int epfd, connection_t *tunnel) {
    if (!tunnel || tunnel->state != CONN_TUNNEL_READY) return;
    if (tunnel->tunnel_id[0] == '\0') return;

    pending_request_t req;
    while (pending_queue_pop(tunnel->tunnel_id, &req) == 0) {
        /* Check browser is still alive */
        connection_t *browser = connection_get(req.fd);
        if (!browser || browser->fd == 0) {
            continue;  /* browser died while queued, try next */
        }

        /* Re-enable EPOLLIN on the browser */
        struct epoll_event ev = { .events = EPOLLIN | EPOLLHUP | EPOLLERR, .data.fd = req.fd };
        epoll_ctl(epfd, EPOLL_CTL_MOD, req.fd, &ev);

        /* Bind and forward */
        connection_bind(req.fd, tunnel->fd);

        if (frame_write(tunnel->fd, FRAME_CONNECT_REQUEST, "NEW", 3) < 0) {
            fprintf(stderr, "[queue] Failed CONNECT_REQUEST: %s\n", strerror(errno));
            connection_close(epfd, req.fd);
            tunnel->peer_fd = 0;
            tunnel->state = CONN_TUNNEL_READY;
            continue;
        }

        if (frame_write(tunnel->fd, FRAME_DATA, req.request, (uint32_t)req.request_len) < 0) {
            fprintf(stderr, "[queue] Failed to forward queued request: %s\n", strerror(errno));
            connection_close(epfd, req.fd);
            tunnel->peer_fd = 0;
            tunnel->state = CONN_TUNNEL_READY;
            continue;
        }

        fprintf(stderr, "[queue] Served queued Browser(fd=%d) on Tunnel(%s)\n",
                req.fd, tunnel->tunnel_id);
        return;  /* successfully served one — tunnel is now FORWARDING */
    }
}

int handle_rift_frame(int fd) {
    frame_type_t type;
    char payload[FRAME_MAX_PAYLOAD + 1];
    uint32_t len;

    if (frame_read(fd, &type, payload, &len) < 0) return -1;

    connection_t *c = connection_get(fd);
    if (!c) return -1;

    if (type == FRAME_REGISTER_TUNNEL) {
        snprintf(c->tunnel_id, sizeof(c->tunnel_id), "%.*s", (int)len, payload);
        c->state = CONN_TUNNEL_READY;
        fprintf(stderr, "[tunnel] Registered: %s (fd=%d)\n", c->tunnel_id, fd);
        return 0;
    }

    if (type == FRAME_CONNECT_REQUEST) {
        char service_id[64] = {0};
        snprintf(service_id, sizeof(service_id), "%.*s", (int)len, payload);
        connection_t *tunnel = connection_find_tunnel(service_id);
        if (tunnel) {
            connection_bind(fd, tunnel->fd);
            c->state = CONN_TUNNEL_READY;
            fprintf(stderr, "[bind] Internal request: %d -> %d (%s)\n", fd, tunnel->fd, service_id);
            return 0;
        }
    }

    return -1;
}
