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

void connection_bind(int browser_fd, int tunnel_fd) {
    connection_t *b = connection_get(browser_fd);
    connection_t *t = connection_get(tunnel_fd);
    if (!b || !t) return;

    b->peer_fd = tunnel_fd;
    t->peer_fd = browser_fd;
    b->state = CONN_PUBLIC_FORWARDING;
    t->state = CONN_TUNNEL_FORWARDING;
}

void connection_close(int epfd, int fd) {
    connection_t *c = connection_get(fd);
    if (!c || c->fd == 0) return;

    int peer = c->peer_fd;
    conn_state_t orig_state = c->state;

    /* If a tunnel is dying, clear its pending queue */
    if ((orig_state == CONN_TUNNEL_INIT || orig_state == CONN_TUNNEL_READY ||
         orig_state == CONN_TUNNEL_FORWARDING) && c->tunnel_id[0] != '\0') {
        pending_queue_clear(c->tunnel_id, epfd);
    }

    c->fd = 0;
    c->peer_fd = 0;
    c->state = 0;
    c->tunnel_id[0] = '\0';
    c->service_id[0] = '\0';
    active_connections--;

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    if (peer > 0) {
        connection_t *p = connection_get(peer);
        if (p && p->peer_fd == fd) {
            p->peer_fd = 0;
            if (orig_state == CONN_TUNNEL_READY || orig_state == CONN_TUNNEL_FORWARDING) {
                /* Tunnel closing - close its paired browser */
                connection_close(epfd, peer);
            } else if (orig_state == CONN_PUBLIC_FORWARDING) {
                /* Browser closing - notify tunnel, reset to READY */
                frame_write(peer, FRAME_CLOSE, "browser_cancelled", 17);
                p->state = CONN_TUNNEL_READY;
                fprintf(stderr, "[connection] Browser closed (fd=%d), tunnel (fd=%d) reset to READY\n",
                        fd, peer);
                /* Immediately serve next queued request */
                serve_next_pending(epfd, p);
            }
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
        if (c->peer_fd != 0) continue;
        if (strcmp(c->tunnel_id, tunnel_id) == 0)
            return c;
    }
    return NULL;
}

connection_t* connection_find_tunnel_any(const char *tunnel_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t *c = &connections[i];
        if (c->fd == 0) continue;
        if (c->state != CONN_TUNNEL_READY && c->state != CONN_TUNNEL_FORWARDING)
            continue;
        if (strcmp(c->tunnel_id, tunnel_id) == 0)
            return c;
    }
    return NULL;
}

/* ---- Pending request queue (flat array, one per tunnel_id) ---- */
#include <stdlib.h>

#define MAX_TUNNEL_QUEUES 256

static struct {
    char tunnel_id[ID_LEN];
    pending_request_t items[MAX_PENDING_PER_TUNNEL];
    int count;
} queues[MAX_TUNNEL_QUEUES];

static int find_queue_idx(const char *tid, int create) {
    int free_slot = -1;
    for (int i = 0; i < MAX_TUNNEL_QUEUES; i++) {
        if (queues[i].tunnel_id[0] == '\0') {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(queues[i].tunnel_id, tid) == 0) return i;
    }
    if (create && free_slot >= 0) {
        snprintf(queues[free_slot].tunnel_id, ID_LEN, "%s", tid);
        queues[free_slot].count = 0;
        return free_slot;
    }
    return -1;
}

int pending_queue_push(const char *tunnel_id, int browser_fd,
                       const char *request, size_t request_len) {
    int qi = find_queue_idx(tunnel_id, 1);
    if (qi < 0 || queues[qi].count >= MAX_PENDING_PER_TUNNEL) return -1;
    if (request_len > PENDING_REQUEST_MAX) return -1;
    pending_request_t *p = &queues[qi].items[queues[qi].count++];
    p->fd = browser_fd;
    memcpy(p->request, request, request_len);
    p->request_len = request_len;
    return 0;
}

int pending_queue_pop(const char *tunnel_id, pending_request_t *out) {
    int qi = find_queue_idx(tunnel_id, 0);
    if (qi < 0 || queues[qi].count == 0) return -1;
    *out = queues[qi].items[0];
    queues[qi].count--;
    for (int i = 0; i < queues[qi].count; i++)
        queues[qi].items[i] = queues[qi].items[i + 1];
    return 0;
}

void pending_queue_clear(const char *tunnel_id, int epfd) {
    int qi = find_queue_idx(tunnel_id, 0);
    if (qi < 0) return;
    const char *msg = "Tunnel disconnected\n";
    char resp[256];
    int len = snprintf(resp, sizeof(resp),
        "HTTP/1.1 502 Bad Gateway\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s", strlen(msg), msg);
    for (int i = 0; i < queues[qi].count; i++) {
        int bfd = queues[qi].items[i].fd;
        if (write(bfd, resp, len) < 0) { /* best effort */ }
        connection_close(epfd, bfd);
    }
    queues[qi].count = 0;
    queues[qi].tunnel_id[0] = '\0';
}
