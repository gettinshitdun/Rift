//
// Created by kanishak on 1/3/26.
//
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BACKLOG 10
#define BUF_SIZE 4096

int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };


    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }

    return fd;
}

void forward(int from, int to) {
    char buf[BUF_SIZE];
    ssize_t n;

    while ((n = read(from, buf, sizeof(buf))) > 0) {
        if (write(to, buf, n) != n) break;
    }
}

int main() {
    int tunnel_listener = create_listener(7000);
    int public_listener = create_listener(9000);

    printf("[server] waiting for tunnel client...\n");
    int tunnel_fd = accept(tunnel_listener, NULL, NULL);
    if (tunnel_fd < 0) { perror("accept tunnel"); exit(1); }
    printf("[server] tunnel connected\n");

    printf("[server] waiting for public connection...\n");
    int public_fd = accept(public_listener, NULL, NULL);
    if (public_fd < 0) { perror("accept public"); exit(1); }
    printf("[server] public client connected\n");

    if (fork() == 0) {
        forward(public_fd, tunnel_fd);
        exit(0);
    } else {
        forward(tunnel_fd, public_fd);
    }

    close(public_fd);
    close(tunnel_fd);
    return 0;
}
