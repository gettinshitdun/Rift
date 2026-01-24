#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <netdb.h>

#include "../server/include/frame.h"

#define MAX_EVENTS 10
#define DEFAULT_SERVER_IP "43.205.120.186"
#define DEFAULT_SERVER_PORT 7000

typedef struct {
    char buf[FRAME_MAX_PAYLOAD + 16];
    size_t len;
} frame_buffer_t;

static frame_buffer_t frame_buf = {0};

void generate_random_id(char *buf, size_t len) {
    const char *adj[] = {"bold", "swift", "calm", "fiery", "dark", "neon"};
    const char *noun[] = {"beast", "wave", "peak", "coder", "rift", "bolt"};
    srand(time(NULL) ^ getpid());
    snprintf(buf, len, "%s-%s-%d", adj[rand() % 6], noun[rand() % 6], rand() % 100);
}

int tcp_connect(const char* ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port) };
    
    // Try direct IP first
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        // Not an IP, try DNS resolution
        struct hostent *host = gethostbyname(ip);
        if (!host) {
            close(fd);
            return -1;
        }
        memcpy(&addr.sin_addr, host->h_addr, host->h_length);
    }

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

int frame_read_buffered(int fd, frame_type_t *type, char *payload, uint32_t *len) {
    while (frame_buf.len < sizeof(frame_buf.buf)) {
        ssize_t n = read(fd, frame_buf.buf + frame_buf.len, sizeof(frame_buf.buf) - frame_buf.len);
        if (n > 0) {
            frame_buf.len += n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        } else {
            errno = ECONNRESET;
            return -1;
        }
    }

    if (frame_buf.len < sizeof(frame_header_t)) {
        errno = EAGAIN;
        return -1;
    }

    // Parse the header
    frame_header_t *hdr = (frame_header_t *)frame_buf.buf;
    uint32_t magic = ntohl(hdr->magic);
    uint8_t version = hdr->version;
    uint16_t h_type = ntohs(hdr->type);
    uint32_t h_len = ntohl(hdr->length);

    if (magic != FRAME_MAGIC) {
        fprintf(stderr, "[frame] Magic mismatch: got 0x%08x, expected 0x%08x\n", magic, FRAME_MAGIC);
        // Corrupted frame - try to skip a byte and resync
        memmove(frame_buf.buf, frame_buf.buf + 1, frame_buf.len - 1);
        frame_buf.len--;
        errno = EBADMSG;
        return -1;
    }

    if (version != FRAME_VERSION) {
        fprintf(stderr, "[frame] Unsupported version: %d (expected %d)\n", version, FRAME_VERSION);
        errno = EPROTO;
        return -1;
    }

    if (h_len > FRAME_MAX_PAYLOAD) {
        fprintf(stderr, "[frame] Payload too large: %u\n", h_len);
        errno = EMSGSIZE;
        return -1;
    }

    // Check if we have the complete frame (header + payload)
    size_t frame_total = sizeof(frame_header_t) + h_len;
    if (frame_buf.len < frame_total) {
        errno = EAGAIN;
        return -1;  // Incomplete frame
    }

    // Extract the frame data
    *type = (frame_type_t)h_type;
    *len = h_len;
    if (h_len > 0) {
        memcpy(payload, frame_buf.buf + sizeof(frame_header_t), h_len);
    }

    // Remove the processed frame from the buffer
    memmove(frame_buf.buf, frame_buf.buf + frame_total, frame_buf.len - frame_total);
    frame_buf.len -= frame_total;

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 3 || strcmp(argv[1], "expose") != 0) {
        fprintf(stderr, "Usage: rift expose <port>\n");
        return 1;
    }

    int local_port = atoi(argv[2]);
    if (local_port <= 0 || local_port > 65535) {
        fprintf(stderr, "Error: Invalid port %d\n", local_port);
        return 1;
    }

    // Allow server address override via environment variable for testing
    const char *server_ip = getenv("RIFT_SERVER_IP");
    if (!server_ip) {
        server_ip = DEFAULT_SERVER_IP;
    }
    int server_port = DEFAULT_SERVER_PORT;
    const char *server_port_env = getenv("RIFT_SERVER_PORT");
    if (server_port_env) {
        server_port = atoi(server_port_env);
    }

    char tunnel_id[64];
    generate_random_id(tunnel_id, sizeof(tunnel_id));

    printf("\n--- RIFT CLIENT v1 ---\n");
    printf("Forwarding: localhost:%d <---> Rift Server (%s:%d)\n", local_port, server_ip, server_port);
    printf("Tunnel ID:  %s\n", tunnel_id);
    printf("-------------------\n\n");

    int server_fd = tcp_connect(server_ip, server_port);
    if (server_fd < 0) {
        perror("Error: Could not connect to Rift Server");
        return 1;
    }

    if (frame_write(server_fd, FRAME_REGISTER_TUNNEL, tunnel_id, (uint32_t)strlen(tunnel_id)) < 0) {
        fprintf(stderr, "Failed to send registration frame\n");
        close(server_fd);
        return 1;
    }

    int local_fd = -1;
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        close(server_fd);
        return 1;
    }

    struct epoll_event ev, events[MAX_EVENTS];

    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            uint32_t revents = events[i].events;
            int fd = events[i].data.fd;

            if (revents & EPOLLIN) {
                if (fd == server_fd) {
                    // Read all available frames from server using buffered reader
                    int frames_read = 0;
                    while (1) {
                        frame_type_t type;
                        char payload[FRAME_MAX_PAYLOAD];
                        uint32_t len;

                        int res = frame_read_buffered(server_fd, &type, payload, &len);
                        if (res < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPROTO || errno == EBADMSG) {
                                if (frames_read == 0 && errno == EAGAIN) {
                                    // Normal condition - just no data available
                                }
                                break; // No more complete frames
                            }
                            fprintf(stderr, "[!] Server connection error: %s\n", strerror(errno));
                            goto cleanup;
                        }
                        frames_read++;

                        // Process the frame
                        if (type == FRAME_CONNECT_REQUEST) {
                            // Close existing local connection if any
                            if (local_fd > 0) {
                                epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                                close(local_fd);
                            }

                            // Connect to local service
                            local_fd = tcp_connect("127.0.0.1", local_port);
                            if (local_fd > 0) {
                                struct epoll_event lev = {.events = EPOLLIN, .data.fd = local_fd};
                                epoll_ctl(epfd, EPOLL_CTL_ADD, local_fd, &lev);
                            } else {
                                perror("[!] Failed to connect to local service");
                            }
                        }
                        else if (type == FRAME_DATA) {
                            if (local_fd > 0) {
                                ssize_t written = write(local_fd, payload, len);
                                if (written < 0) {
                                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                        perror("[!] Write to local service failed");
                                    }
                                } else if (written != (ssize_t)len) {
                                    fprintf(stderr, "[!] Partial write: %ld/%u bytes\n", written, len);
                                }
                            } else {
                                printf("[!] Dropped %u bytes (no local connection)\n", len);
                            }
                        }
                        else if (type == FRAME_CLOSE) {
                            if (local_fd > 0) {
                                printf("[*] Local connection closed\n");
                                epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                                close(local_fd);
                                local_fd = -1;
                            }
                        }
                        else {
                            fprintf(stderr, "[!] Unknown frame type: %d\n", type);
                        }
                    }
                }
                else if (fd == local_fd) {
                    // Read all available data from local service
                    char buffer[FRAME_MAX_PAYLOAD];
                    while (1) {
                        ssize_t n = read(local_fd, buffer, sizeof(buffer));
                        if (n > 0) {
                            if (frame_write(server_fd, FRAME_DATA, buffer, (uint32_t)n) < 0) {
                                fprintf(stderr, "[!] Failed to send to server: %s\n", strerror(errno));
                                goto cleanup;
                            }
                        } else if (n < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break; // No more data
                            }
                            perror("[!] Read error from local service");
                            break;
                        } else {
                            // Connection closed - send FRAME_CLOSE to notify server
                            if (frame_write(server_fd, FRAME_CLOSE, NULL, 0) < 0) {
                                fprintf(stderr, "[!] Failed to notify server of close: %s\n", strerror(errno));
                            }
                            epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                            close(local_fd);
                            local_fd = -1;
                            break;
                        }
                    }
                }
            }

            if (revents & (EPOLLHUP | EPOLLERR)) {
                if (fd == server_fd) {
                    printf("[!] Server connection lost\n");
                    goto cleanup;
                } else if (fd == local_fd) {
                    printf("[*] Local connection closed\n");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                    close(local_fd);
                    local_fd = -1;
                }
            }
        }
    }

cleanup:
    printf("\n[*] Shutting down...\n");
    close(server_fd);
    if (local_fd > 0) close(local_fd);
    close(epfd);
    return 0;
}