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
    if (!c) {
        close(fd);
        return;
    }
    c->state = CONN_TUNNEL_INIT;
}

void connection_add_public(int fd) {
    connection_t *c = connection_alloc(fd);
    if (!c) {
        close(fd);
        return;
    }
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
                connection_close(epfd, peer);
            } else if (orig_state == CONN_PUBLIC_INIT || orig_state == CONN_PUBLIC_FORWARDING) {
                if (frame_write(peer, FRAME_CLOSE, NULL, 0) < 0) {
                    fprintf(stderr, "[connection] Failed to send FRAME_CLOSE to tunnel (fd=%d): %s\n", peer, strerror(errno));
                }
                p->state = CONN_TUNNEL_READY;
            }
        }
    }
}

void connection_reset_public(int fd) {
    connection_t *c = connection_get(fd);
    if (!c || c->fd == 0) return;

    if (c->state == CONN_PUBLIC_FORWARDING) {
        c->peer_fd = 0;
        c->state = CONN_PUBLIC_INIT;
        c->service_id[0] = '\0';
        fprintf(stderr, "[connection] Reset public connection (fd=%d) back to INIT state for next request\n", fd);
    }
}

int connection_active_count(void) {
    return active_connections;
}

connection_t* connection_find_tunnel(const char *tunnel_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connection_t *c = &connections[i];

        if (c->fd == 0)
            continue;

        if (c->state != CONN_TUNNEL_READY)
            continue;

        if (c->peer_fd != 0)
            continue;

        if (strcmp(c->tunnel_id, tunnel_id) == 0)
            return c;
    }
    return NULL;
}

