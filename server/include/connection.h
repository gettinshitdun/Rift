#ifndef RIFT_CONNECTION_H
#define RIFT_CONNECTION_H

#include <stdint.h>
#include <stddef.h>
#include "frame.h"

#define MAX_CONNECTIONS 102400
#define ID_LEN 64

/* Per-connection read buffer size: header + max payload + slack */
#define CONN_RBUF_SIZE (sizeof(frame_header_t) + FRAME_MAX_PAYLOAD + 64)

/* Maximum concurrent streams (browser connections) per tunnel */
#define MAX_STREAMS_PER_TUNNEL 128

typedef enum {
    CONN_TUNNEL_INIT,
    CONN_TUNNEL_READY,       /* Tunnel registered, can accept streams */
    CONN_PUBLIC_INIT,
    CONN_PUBLIC_FORWARDING,  /* Browser linked to a stream on a tunnel */
} conn_state_t;

/* Stream map entry: stream_id <-> browser_fd */
typedef struct {
    uint32_t stream_id;
    int      browser_fd;
} stream_entry_t;

typedef struct {
    int fd;
    conn_state_t state;
    int peer_fd;              /* For browsers: tunnel_fd.  For tunnels: unused (0) */
    uint32_t stream_id;      /* For browsers: which stream they are on */
    char tunnel_id[ID_LEN];
    char service_id[ID_LEN];

    /* Stream map — only used by tunnel connections */
    stream_entry_t streams[MAX_STREAMS_PER_TUNNEL];
    int stream_count;

    /* Per-connection read buffer for non-blocking frame reassembly.
       Prevents partial-read data loss across epoll_wait cycles. */
    char  *rbuf;       /* heap-allocated on first tunnel use */
    size_t rbuf_len;   /* bytes currently buffered */
} connection_t;

void connection_init(void);
void connection_add_tunnel(int fd);
void connection_add_public(int fd);
connection_t* connection_get(int fd);
void connection_close(int epfd, int fd);
int  connection_active_count(void);

/* Find a READY tunnel by tunnel_id */
connection_t* connection_find_tunnel(const char *tunnel_id);

/* Stream map operations on a tunnel */
uint32_t tunnel_next_stream_id(connection_t *tunnel);
int      tunnel_add_stream(connection_t *tunnel, uint32_t stream_id, int browser_fd);
int      tunnel_find_browser(connection_t *tunnel, uint32_t stream_id);
void     tunnel_remove_stream(connection_t *tunnel, uint32_t stream_id);
void     tunnel_close_all_streams(connection_t *tunnel, int epfd);

/* Buffered frame read: accumulates partial data in c->rbuf across calls.
   Returns 0 on success, -1 on error (check errno: EAGAIN = incomplete). */
int connection_frame_read(connection_t *c, frame_type_t *type,
                          char *payload, uint32_t *len, uint32_t *stream_id);

#endif
