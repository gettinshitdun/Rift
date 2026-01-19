#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "include/handlers.h"
#include "include/connection.h"
#include "include/frame.h"

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

    // 1. Try to find custom header X-Tunnel-Id
    char *custom_hdr = strstr(peek_buf, "X-Tunnel-Id: ");
    if (custom_hdr) {
        sscanf(custom_hdr + 13, "%63s", tunnel_id);
    } 
    // 2. Fallback: Parse Host header
    else {
        char *host_hdr = strstr(peek_buf, "Host: ");
        if (host_hdr) {
            sscanf(host_hdr + 6, "%63[^.:\r\n]", tunnel_id);
        }
    }

    if (tunnel_id[0] != '\0') {
        connection_t *tunnel = connection_find_tunnel(tunnel_id);
        if (tunnel) {
            // Link the browser connection to the tunnel connection
            connection_bind(fd, tunnel->fd);


            // STEP 1: Alert the Rift Client that a new request is starting
            // (Optional but good for client-side logging)
            frame_write(tunnel->fd, FRAME_CONNECT_REQUEST, "NEW_CONN", 8);

            // STEP 2: FORWARD THE STOLEN BUFFER
            // Because 'peek_buf' was already read from the socket, the main loop 
            // won't see it. We must manually wrap it and send it now.
            if (frame_write(tunnel->fd, FRAME_DATA, peek_buf, strlen(peek_buf)) < 0) {
                printf("[http] Failed to forward initial request to tunnel\n");
                return -1;
            }

            printf("[http] Linked Browser(fd=%d) to Tunnel(%s) and forwarded header\n", fd, tunnel_id);
            return 0;
        }
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
    uint16_t len;

    // frame_read handles the binary header (8 bytes) and consumes it from the socket
    if (frame_read(fd, &type, payload, &len) < 0) return -1;

    connection_t *c = connection_get(fd);
    if (!c) return -1;

    // Scenario A: Standard registration from client
    if (type == FRAME_REGISTER_TUNNEL) {
        snprintf(c->tunnel_id, sizeof(c->tunnel_id), "%.*s", len, payload);
        
        // TRANSITION STATE: Move to active tunnel mode
        c->state = CONN_TUNNEL_READY; 
        
        printf("[tunnel] Registered: %s (fd=%d)\n", c->tunnel_id, fd);
        return 0;
    } 
    
    // Scenario B: Client requesting a bind to an existing service
    if (type == FRAME_CONNECT_REQUEST) {
        char service_id[64] = {0};
        snprintf(service_id, sizeof(service_id), "%.*s", len, payload);
        connection_t *tunnel = connection_find_tunnel(service_id);
        if (tunnel) {
            connection_bind(fd, tunnel->fd);
            c->state = CONN_TUNNEL_READY;
            printf("[bind] Internal request: %d -> %d (%s)\n", fd, tunnel->fd, service_id);
            return 0;
        }
    }
    
    return -1;
}