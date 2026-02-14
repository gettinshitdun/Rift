#ifndef RIFT_CONNECTION_H
#define RIFT_CONNECTION_H

#include <stdint.h>

#define MAX_CONNECTIONS 102400
#define ID_LEN 64

typedef enum {
    CONN_TUNNEL_INIT,
    CONN_TUNNEL_READY,
    CONN_TUNNEL_FORWARDING,
    CONN_PUBLIC_INIT,
    CONN_PUBLIC_FORWARDING,
} conn_state_t;

typedef struct {
    int fd;
    conn_state_t state;
    int peer_fd;
    char tunnel_id[ID_LEN];
    char service_id[ID_LEN];
} connection_t;

void connection_init(void);
void connection_add_tunnel(int fd);
void connection_add_public(int fd);
connection_t* connection_get(int fd);
void connection_bind(int fd1, int fd2);
void connection_close(int epfd, int fd);
int  connection_active_count(void);
connection_t* connection_find_tunnel(const char *tunnel_id);

#endif
