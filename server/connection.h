//
// Created by kanishak on 1/4/26.
//
// server/connection.h

#ifndef RIFT_CONNECTION_H
#define RIFT_CONNECTION_H

#define MAX_CONNECTIONS 8192

typedef struct {
    int fd;
    int peer_fd;
} connection_t;

// Initialize connection table
void connection_init(void);

// Register a new connection
void connection_add(int fd);

// Pair two connections
void connection_pair(int fd1, int fd2);

// Get peer fd (or -1)
int connection_get_peer(int fd);

// Close connection and its peer
void connection_close(int epfd, int fd);


#endif //RIFT_CONNECTION_H