#ifndef RIFT_CONNECTION_H
#define RIFT_CONNECTION_H

#define MAX_CONNECTIONS 102400

typedef struct {
    int fd;
    int peer_fd;   // fd this connection forwards to
    int type;      // 0 = tunnel, 1 = public
} connection_t;

// Initialize connection table
void connection_init(void);

// Add a new client
void connection_add(int fd, int type);

// Get peer
int connection_get_peer(int fd);

// Set peer
void connection_set_peer(int fd, int peer_fd);

// Close connection
void connection_close(int epfd, int fd);

// Get first unpaired client of a type
int connection_get_unpaired(int type);

int connection_active_count();
#endif
