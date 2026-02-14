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

/**
 * handle_http_request — multiplexed version.
 *
 * Assigns a new stream_id, registers it in the tunnel's stream map,
 * then sends CONNECT_REQUEST + FRAME_DATA to the tunnel.
 * Multiple browsers can be active on the same tunnel simultaneously.
 *
 * Returns  0 = success (browser linked to stream)
 *         -1 = error (caller should close browser)
 */
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
            /* Allocate a new stream for this browser */
            uint32_t sid = tunnel_next_stream_id(tunnel);

            if (tunnel_add_stream(tunnel, sid, fd) < 0) {
                fprintf(stderr, "[http] Max streams reached for tunnel %s\n", tunnel_id);
                send_http_error(fd, "503 Service Unavailable", "Too many concurrent requests\n");
                return -1;
            }

            /* Link browser to this tunnel + stream */
            connection_t *browser = connection_get(fd);
            if (browser) {
                browser->peer_fd = tunnel->fd;
                browser->stream_id = sid;
                browser->state = CONN_PUBLIC_FORWARDING;
            }

            /* Send CONNECT_REQUEST with stream_id so client opens a new local conn */
            if (frame_write(tunnel->fd, FRAME_CONNECT_REQUEST, "NEW", 3, sid) < 0) {
                fprintf(stderr, "[http] Failed to write FRAME_CONNECT_REQUEST: %s\n", strerror(errno));
                tunnel_remove_stream(tunnel, sid);
                return -1;
            }

            /* Forward the HTTP request as FRAME_DATA tagged with stream_id */
            size_t hdr_len = strlen(peek_buf);
            if (hdr_len > FRAME_MAX_PAYLOAD) {
                fprintf(stderr, "[http] HTTP request too large: %zu bytes\n", hdr_len);
                tunnel_remove_stream(tunnel, sid);
                return -1;
            }

            if (frame_write(tunnel->fd, FRAME_DATA, peek_buf, (uint32_t)hdr_len, sid) < 0) {
                fprintf(stderr, "[http] Failed to forward request: %s\n", strerror(errno));
                tunnel_remove_stream(tunnel, sid);
                return -1;
            }

            fprintf(stderr, "[http] Stream %u: Browser(fd=%d) -> Tunnel(%s), %zu bytes\n",
                    sid, fd, tunnel_id, hdr_len);
            return 0;
        }

        fprintf(stderr, "[http] Tunnel not found: %s\n", tunnel_id);
    } else {
        fprintf(stderr, "[http] No tunnel ID in request\n");
    }

    send_http_error(fd, "404 Not Found", "No Tunnel Specified or Found\n");
    return -1;
}

int handle_rift_frame(int fd) {
    frame_type_t type;
    char payload[FRAME_MAX_PAYLOAD + 1];
    uint32_t len;
    uint32_t stream_id;

    if (frame_read(fd, &type, payload, &len, &stream_id) < 0) return -1;

    connection_t *c = connection_get(fd);
    if (!c) return -1;

    if (type == FRAME_REGISTER_TUNNEL) {
        snprintf(c->tunnel_id, sizeof(c->tunnel_id), "%.*s", (int)len, payload);
        c->state = CONN_TUNNEL_READY;
        c->stream_count = 0;
        fprintf(stderr, "[tunnel] Registered: %s (fd=%d)\n", c->tunnel_id, fd);
        return 0;
    }

    return -1;
}
