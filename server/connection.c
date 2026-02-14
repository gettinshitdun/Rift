#include "include/connection.h"
#include "include/frame.h"
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/epoll.h>

static connection_t connections[MAX_CONNECTIONS];
static int active_connections = 0;

void connection_init(void) {
    memset(connections, 0, sizeof(connections));
    active_connections = 0;
}

static connection_t* connection_alloc(int fd) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].fd == 0) {
            memset(&connections[i], 0, sizeof(connection_t));
            connections[i].fd = fd;
            active_connections++;
            return &connections[i];
        }
    }
    return NULL;
}

connection_t* connection_get(int fd) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].fd == fd)
            return &connections[i];
    }
    return NULL;
}

void connection_add_tunnel(int fd) {
    connection_t *c = connection_alloc(fd);
    if (!c) { close(fd); return; }
    c->state = CONN_TUNNEL_INIT;
}

void connection_add_public(int fd) {
    connection_t *c = connection_alloc(fd);
    if (!c) { close(fd); return; }
    c->state = CONN_PUBLIC_INIT;
}

void connection_close(int epfd, int fd) {
    connection_t *c = connection_get(fd);
    if (!c || c->fd == 0) return;

    conn_state_t orig_state = c->state;
    int tunnel_fd = c->peer_fd;
    uint32_t sid = c->stream_id;

    /* If a tunnel is dying, close all its active browser streams */
    if (orig_state == CONN_TUNNEL_INIT || orig_state == CONN_TUNNEL_READY) {
        tunnel_close_all_streams(c, epfd);
    }

    c->fd = 0;
    c->peer_fd = 0;
    c->state = 0;
    c->stream_id = 0;
    c->stream_count = 0;
    c->tunnel_id[0] = '\0';
    c->service_id[0] = '\0';
    active_connections--;

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    /* If a browser dies, remove it from the tunnel's stream map and notify */
    if (orig_state == CONN_PUBLIC_FORWARDING && tunnel_fd > 0 && sid > 0) {
        connection_t *t = connection_get(tunnel_fd);
        if (t && t->fd != 0) {
            tunnel_remove_stream(t, sid);
            /* Notify client that this stream's browser is gone */
            frame_write(tunnel_fd, FRAME_CLOSE, "browser_closed", 14, sid);
            fprintf(stderr, "[connection] Browser fd=%d stream=%u closed, notified tunnel fd=%d\n",
                    fd, sid, tunnel_fd);
        }
    }
}

int connection_active_count(void) {
    return active_connections;
}

connection_t* connection_find_tunnel(const char *tunnel_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t *c = &connections[i];
        if (c->fd == 0) continue;
        if (c->state != CONN_TUNNEL_READY) continue;
        if (strcmp(c->tunnel_id, tunnel_id) == 0)
            return c;
    }
    return NULL;
}

/* ---- Stream map operations ---- */

uint32_t tunnel_next_stream_id(connection_t *tunnel) {
    (void)tunnel;  /* stream IDs are global, not per-tunnel */
    /* Simple incrementing counter; wraps at UINT32_MAX.
       Stream 0 is reserved for control frames. */
    static uint32_t global_stream_counter = 0;
    return ++global_stream_counter;
}

int tunnel_add_stream(connection_t *tunnel, uint32_t stream_id, int browser_fd) {
    if (!tunnel || tunnel->stream_count >= MAX_STREAMS_PER_TUNNEL) return -1;
    stream_entry_t *e = &tunnel->streams[tunnel->stream_count++];
    e->stream_id = stream_id;
    e->browser_fd = browser_fd;
    return 0;
}

int tunnel_find_browser(connection_t *tunnel, uint32_t stream_id) {
    if (!tunnel) return -1;
    for (int i = 0; i < tunnel->stream_count; i++) {
        if (tunnel->streams[i].stream_id == stream_id)
            return tunnel->streams[i].browser_fd;
    }
    return -1;
}

void tunnel_remove_stream(connection_t *tunnel, uint32_t stream_id) {
    if (!tunnel) return;
    for (int i = 0; i < tunnel->stream_count; i++) {
        if (tunnel->streams[i].stream_id == stream_id) {
            /* Swap with last element for O(1) removal */
            tunnel->streams[i] = tunnel->streams[tunnel->stream_count - 1];
            tunnel->stream_count--;
            return;
        }
    }
}

void tunnel_close_all_streams(connection_t *tunnel, int epfd) {
    if (!tunnel) return;
    /* Close all browsers connected through this tunnel */
    for (int i = 0; i < tunnel->stream_count; i++) {
        int bfd = tunnel->streams[i].browser_fd;
        connection_t *b = connection_get(bfd);
        if (b && b->fd != 0) {
            /* Prevent cascade back into tunnel (it's already dying) */
            b->peer_fd = 0;
            b->stream_id = 0;
            /* Send 502 to browser */
            const char *msg = "Tunnel disconnected\n";
            char resp[256];
            int rlen = snprintf(resp, sizeof(resp),
                "HTTP/1.1 502 Bad Gateway\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "\r\n"
                "%s", strlen(msg), msg);
            if (write(bfd, resp, rlen) < 0) { /* best effort */ }
            /* Close the browser connection */
            b->fd = 0;
            b->state = 0;
            active_connections--;
            epoll_ctl(epfd, EPOLL_CTL_DEL, bfd, NULL);
            close(bfd);
        }
    }
    tunnel->stream_count = 0;
}
