#ifndef RIFT_CONNECTION_H
#define RIFT_CONNECTION_H

#include <stdint.h>
#include <stddef.h>

#define MAX_CONNECTIONS 102400
#define ID_LEN 64

typedef enum {
    CONN_TUNNEL_INIT,
    CONN_TUNNEL_READY,
    CONN_TUNNEL_FORWARDING,
    CONN_PUBLIC_INIT,
    CONN_PUBLIC_FORWARDING,
    CONN_PUBLIC_QUEUED,
} conn_state_t;

typedef struct {
    int fd;
    conn_state_t state;
    int peer_fd;
    char tunnel_id[ID_LEN];
    char service_id[ID_LEN];
} connection_t;

/* Pending request queue — holds browser requests while tunnel is busy */
#define MAX_PENDING_PER_TUNNEL 64
#define PENDING_REQUEST_MAX 16384

typedef struct {
    int fd;
    char request[PENDING_REQUEST_MAX];
    size_t request_len;
} pending_request_t;

void connection_init(void);
void connection_add_tunnel(int fd);
void connection_add_public(int fd);
connection_t* connection_get(int fd);
void connection_bind(int fd1, int fd2);
void connection_close(int epfd, int fd);
int  connection_active_count(void);
connection_t* connection_find_tunnel(const char *tunnel_id);
connection_t* connection_find_tunnel_any(const char *tunnel_id);

/* Pending queue operations */
int  pending_queue_push(const char *tunnel_id, int browser_fd,
                        const char *request, size_t request_len);
int  pending_queue_pop(const char *tunnel_id, pending_request_t *out);
void pending_queue_clear(const char *tunnel_id, int epfd);

/* Serve next queued browser when tunnel becomes READY */
void serve_next_pending(int epfd, connection_t *tunnel);

#endif
