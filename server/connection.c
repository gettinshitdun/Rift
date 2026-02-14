#include "include/connection.h"
#include "include/frame.h"
#include <string.h>
#include <stdlib.h>
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

    /* Free per-connection read buffer */
    if (c->rbuf) {
        free(c->rbuf);
        c->rbuf = NULL;
    }
    c->rbuf_len = 0;

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
/* ---- Buffered frame read for non-blocking tunnel fds ---- */

int connection_frame_read(connection_t *c, frame_type_t *type,
                          char *payload, uint32_t *len, uint32_t *stream_id)
{
    /* Lazy-allocate the read buffer on first use */
    if (!c->rbuf) {
        c->rbuf = malloc(CONN_RBUF_SIZE);
        if (!c->rbuf) {
            errno = ENOMEM;
            return -1;
        }
        c->rbuf_len = 0;
    }

    /* 1. Drain as much data as the kernel has ready */
    while (c->rbuf_len < CONN_RBUF_SIZE) {
        ssize_t n = read(c->fd, c->rbuf + c->rbuf_len,
                         CONN_RBUF_SIZE - c->rbuf_len);
        if (n > 0) {
            c->rbuf_len += n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return -1;  /* real error */
        } else {
            /* n == 0: peer closed */
            errno = ECONNRESET;
            return -1;
        }
    }

    /* 2. Need at least a full header */
    if (c->rbuf_len < sizeof(frame_header_t)) {
        errno = EAGAIN;
        return -1;
    }

    /* 3. Parse header from buffer */
    frame_header_t *hdr = (frame_header_t *)c->rbuf;
    uint32_t magic   = ntohl(hdr->magic);
    uint8_t  version = hdr->version;
    uint16_t h_type  = ntohs(hdr->type);
    uint32_t h_len   = ntohl(hdr->length);
    uint32_t h_sid   = ntohl(hdr->stream_id);

    if (magic != FRAME_MAGIC) {
        fprintf(stderr, "[frame] Magic mismatch: got 0x%08x, expected 0x%08x\n",
                magic, FRAME_MAGIC);
        fprintf(stderr, "[frame] rbuf_len=%zu, first 32 bytes:", c->rbuf_len);
        for (size_t i = 0; i < c->rbuf_len && i < 32; ++i) fprintf(stderr, " %02x", (unsigned char)c->rbuf[i]);
        fprintf(stderr, "\n");
        // Try to resync: skip one byte
        memmove(c->rbuf, c->rbuf + 1, c->rbuf_len - 1);
        c->rbuf_len--;
        fprintf(stderr, "[frame] After memmove, rbuf_len=%zu, first 32 bytes:", c->rbuf_len);
        for (size_t i = 0; i < c->rbuf_len && i < 32; ++i) fprintf(stderr, " %02x", (unsigned char)c->rbuf[i]);
        fprintf(stderr, "\n");
        errno = EBADMSG;
        return -1;
    }

    if (version != FRAME_VERSION) {
        fprintf(stderr, "[frame] Unsupported version: %d (expected %d)\n",
                version, FRAME_VERSION);
        errno = EPROTO;
        return -1;
    }

    if (h_len > FRAME_MAX_PAYLOAD) {
        fprintf(stderr, "[frame] Payload too large: %u (max %u)\n",
                h_len, FRAME_MAX_PAYLOAD);
        errno = EMSGSIZE;
        return -1;
    }

    /* 4. Need header + full payload */
    size_t frame_total = sizeof(frame_header_t) + h_len;
    if (c->rbuf_len < frame_total) {
        errno = EAGAIN;
        return -1;  /* incomplete payload, wait for more data */
    }

    /* 5. Extract frame */
    *type      = (frame_type_t)h_type;
    *len       = h_len;
    *stream_id = h_sid;
    if (h_len > 0) {
        memcpy(payload, c->rbuf + sizeof(frame_header_t), h_len);
    }

    /* 6. Remove consumed frame from buffer */
    memmove(c->rbuf, c->rbuf + frame_total, c->rbuf_len - frame_total);
    c->rbuf_len -= frame_total;

    return 0;
}