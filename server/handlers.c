#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include "include/handlers.h"
#include "include/connection.h"
#include "include/frame.h"

/* --- Random ID generation --- */
static const char *CHARSET = "abcdefghijklmnopqrstuvwxyz0123456789";

void generate_random_tunnel_id(char *buffer, size_t len) {
    static int seeded = 0;
    if (!seeded) {
        srand(time(NULL));
        seeded = 1;
    }
    for (size_t i = 0; i < len - 1; i++) {
        buffer[i] = CHARSET[rand() % 36];
    }
    buffer[len - 1] = '\0';
}

/* --- Helpers --- */

/**
 * Sends a standard HTTP error response to a browser if a tunnel is missing.
 */
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

/* --- Handlers --- */

/**
 * Handles the initial HTTP request from a browser.
 * Extracts the Tunnel ID from the 'Host' header or 'X-Tunnel-Id'.
 */
int handle_http_request(int fd, const char *peek_buf) {
    char tunnel_id[64] = {0};

    char *custom_hdr = strstr(peek_buf, "x-tunnel-id: ");
    if (!custom_hdr) {
        custom_hdr = strstr(peek_buf, "X-Tunnel-Id: ");
    }
    if (custom_hdr) {
        const char *start = strchr(custom_hdr, ':') + 2;
        sscanf(start, "%63s", tunnel_id);
    }
    else {
        char *host_hdr = strstr(peek_buf, "Host: ");
        if (host_hdr) {
            sscanf(host_hdr + 6, "%63[^.:\r\n]", tunnel_id);
        }
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

            fprintf(stderr, "[http] Linked Browser(fd=%d) to Tunnel(%s), sent %zu bytes\n", fd, tunnel_id, hdr_len);
            return 0;
        } else {
            fprintf(stderr, "[http] Tunnel not found: %s\n", tunnel_id);
        }
    } else {
        fprintf(stderr, "[http] No tunnel ID in request\n");
    }

    send_http_error(fd, "404 Not Found", "No Tunnel Specified or Found\n");
    return -1;
}
/**
 * Handles the initial binary frame from the Rift Client.
 * Consumes the 'RIFT' header and registers the tunnel ID.
 */
int handle_rift_frame(int fd) {
    frame_type_t type;
    char payload[FRAME_MAX_PAYLOAD + 1];
    uint32_t len;

    // frame_read handles the binary header (12 bytes) and consumes it from the socket
    if (frame_read(fd, &type, payload, &len) < 0) return -1;

    connection_t *c = connection_get(fd);
    if (!c) return -1;

    // Scenario A: Standard registration from client
    if (type == FRAME_REGISTER_TUNNEL) {
        // If client provides a tunnel ID, use it; otherwise generate one
        if (len > 0) {
            snprintf(c->tunnel_id, sizeof(c->tunnel_id), "%.*s", (int)len, payload);
        } else {
            // Generate a random 8-character tunnel ID
            generate_random_tunnel_id(c->tunnel_id, 9);
        }

        // TRANSITION STATE: Move to active tunnel mode
        c->state = CONN_TUNNEL_READY;

        // Send back the assigned tunnel ID to the client via FRAME_TUNNEL_READY
        if (frame_write(fd, FRAME_TUNNEL_READY, c->tunnel_id, strlen(c->tunnel_id)) < 0) {
            fprintf(stderr, "[tunnel] Failed to send tunnel assignment: %s\n", strerror(errno));
            return -1;
        }

        fprintf(stderr, "[tunnel] Registered: %s (fd=%d)\n", c->tunnel_id, fd);
        return 0;
    }

    // Scenario B: Client requesting a bind to an existing service
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