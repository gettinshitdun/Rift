//
// Created by kanishak on 1/3/26.
//

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define BUF_SIZE 4096

int connect_to_server(const char* server_ip, int server_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(server_port)
    };

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton"); exit(1);
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); exit(1);
    }

    return fd;
}

int connect_to_local_service(int local_port) {
    return connect_to_server("127.0.0.1", local_port);
}

void forward(int from, int to) {
    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(from, buf, sizeof(buf))) > 0) {
        if (write(to, buf, n) != n) break;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_ip> <server_tunnel_port> <local_service_port>\n", argv[0]);
        return 1;
    }

    const char* server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int local_port  = atoi(argv[3]);

    printf("[client] connecting to server %s:%d...\n", server_ip, server_port);
    int server_fd = connect_to_server(server_ip, server_port);
    printf("[client] connected to server\n");

    printf("[client] connecting to local service on port %d...\n", local_port);
    int local_fd = connect_to_local_service(local_port);
    printf("[client] connected to local service\n");

    if (fork() == 0) {
        forward(local_fd, server_fd); // Local -> Server
        exit(0);
    } else {
        forward(server_fd, local_fd); // Server -> Local
    }

    close(local_fd);
    close(server_fd);
    return 0;
}
