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
#include <sys/random.h>

#include "../server/include/frame.h"

#define MAX_EVENTS 10
#define DEFAULT_SERVER_IP "43.205.120.186"
#define DEFAULT_SERVER_PORT 7000
#define MAX_RECONNECT_ATTEMPTS 5
#define RECONNECT_DELAY_BASE 2  // seconds

#define LOG(fmt, ...) fprintf(stderr, "[LOG] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

typedef struct {
    char buf[FRAME_MAX_PAYLOAD + 16];
    size_t len;
} frame_buffer_t;

static frame_buffer_t frame_buf = {0};

void generate_random_id(char *buf, size_t len) {
    const char *adj[] = {"bold", "swift", "calm", "fiery", "dark", "neon", "silver", "golden", "arctic", "cosmic"};
    const char *noun[] = {"beast", "wave", "peak", "coder", "rift", "bolt", "phoenix", "dragon", "titan", "sage"};
    
    unsigned int rand_val = 0;
    ssize_t ret = getrandom(&rand_val, sizeof(rand_val), GRND_NONBLOCK);
    if (ret <= 0) {
        srand((unsigned int)(time(NULL) ^ getpid()));
        rand_val = rand();
    }
    
    time_t now = time(NULL);
    unsigned int ts_hash = (unsigned int)now ^ getpid() ^ rand_val;
    
    snprintf(buf, len, "%s-%s-%x", adj[rand_val % 10], noun[(rand_val >> 16) % 10], ts_hash & 0xFFFFFF);
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

int reconnect_to_server(const char *server_ip, int server_port, const char *tunnel_id, int epfd) {
    for (int attempt = 1; attempt <= MAX_RECONNECT_ATTEMPTS; attempt++) {
        LOG("Reconnection attempt %d/%d to server %s:%d", attempt, MAX_RECONNECT_ATTEMPTS, server_ip, server_port);
        
        int server_fd = tcp_connect(server_ip, server_port);
        if (server_fd < 0) {
            ERR("Reconnection attempt %d failed: %s", attempt, strerror(errno));
            if (attempt < MAX_RECONNECT_ATTEMPTS) {
                int delay = RECONNECT_DELAY_BASE * attempt;
                LOG("Waiting %d seconds before next attempt...", delay);
                sleep(delay);
            }
            continue;
        }
        
        LOG("Reconnected to server, fd=%d", server_fd);
        
        // Send registration frame
        LOG("Re-registering tunnel: %s", tunnel_id);
        if (frame_write(server_fd, FRAME_REGISTER_TUNNEL, tunnel_id, (uint32_t)strlen(tunnel_id)) < 0) {
            ERR("Failed to re-register tunnel on attempt %d", attempt);
            close(server_fd);
            if (attempt < MAX_RECONNECT_ATTEMPTS) {
                int delay = RECONNECT_DELAY_BASE * attempt;
                sleep(delay);
            }
            continue;
        }
        
        LOG("Re-registration successful");
        
        // Add to epoll
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = server_fd};
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) < 0) {
            ERR("Failed to add reconnected server fd to epoll: %s", strerror(errno));
            close(server_fd);
            continue;
        }
        
        // Clear any buffered data from previous connection
        memset(&frame_buf, 0, sizeof(frame_buf));
        
        LOG("Server reconnection successful");
        return server_fd;
    }
    
    ERR("Failed to reconnect to server after %d attempts", MAX_RECONNECT_ATTEMPTS);
    return -1;
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
    printf("Forwarding: localhost:%d <---> Rift Server\n", local_port);
    printf("Tunnel ID:  %s\n", tunnel_id);
    printf("\n📡 Public URLs:\n");
    printf("   http://%s.rift.kanishakmittal.site\n", tunnel_id);
    printf("   https://%s.rift.kanishakmittal.site\n", tunnel_id);
    printf("-------------------\n\n");

    int server_fd = tcp_connect(server_ip, server_port);
    if (server_fd < 0) {
        ERR("Could not connect to Rift Server");
        return 1;
    }
    LOG("Connected to server, fd=%d", server_fd);

    LOG("Sending registration frame for tunnel: %s", tunnel_id);
    if (frame_write(server_fd, FRAME_REGISTER_TUNNEL, tunnel_id, (uint32_t)strlen(tunnel_id)) < 0) {
        ERR("Failed to send registration frame");
        close(server_fd);
        return 1;
    }
    LOG("Registration successful");

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

    LOG("Event loop started");
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            ERR("epoll_wait failed: %s", strerror(errno));
            break;
        }

        LOG("epoll returned %d events", nfds);
        for (int i = 0; i < nfds; i++) {
            uint32_t revents = events[i].events;
            int fd = events[i].data.fd;

            if (revents & EPOLLIN) {
                if (fd == server_fd) {
                    // Read all available frames from server using buffered reader
                    while (1) {
                        frame_type_t type;
                        char payload[FRAME_MAX_PAYLOAD];
                        uint32_t len;

                        int res = frame_read_buffered(server_fd, &type, payload, &len);
                        if (res < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPROTO || errno == EBADMSG) {
                                LOG("No more frames available");
                                break;
                            }
                            if (errno == ECONNRESET || errno == EPIPE) {
                                ERR("Server connection lost: %s", strerror(errno));
                                
                                // Remove old server fd from epoll
                                epoll_ctl(epfd, EPOLL_CTL_DEL, server_fd, NULL);
                                close(server_fd);
                                
                                // Close any existing local connection
                                if (local_fd > 0) {
                                    LOG("Closing local connection during reconnection");
                                    epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                                    close(local_fd);
                                    local_fd = -1;
                                }
                                
                                // Attempt to reconnect
                                server_fd = reconnect_to_server(server_ip, server_port, tunnel_id, epfd);
                                if (server_fd < 0) {
                                    ERR("Failed to reconnect to server, shutting down");
                                    goto cleanup;
                                }
                                break;
                            }
                            WARN("Frame read error: %s (continuing)", strerror(errno));
                            break;
                        }
                        LOG("Frame received: type=%d, len=%u", type, len);

                        // Process the frame
                        if (type == FRAME_CONNECT_REQUEST) {
                            LOG("FRAME_CONNECT_REQUEST received");
                            // Close existing local connection if any
                            if (local_fd > 0) {
                                LOG("Closing existing local connection fd=%d", local_fd);
                                epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                                close(local_fd);
                            }

                            // Connect to local service
                            LOG("Connecting to local service on port %d", local_port);
                            local_fd = tcp_connect("127.0.0.1", local_port);
                            if (local_fd > 0) {
                                LOG("Local connection established fd=%d", local_fd);
                                struct epoll_event lev = {.events = EPOLLIN, .data.fd = local_fd};
                                epoll_ctl(epfd, EPOLL_CTL_ADD, local_fd, &lev);
                            } else {
                                ERR("Failed to connect to local service: %s", strerror(errno));
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
                            LOG("FRAME_CLOSE received from server");
                            if (local_fd > 0) {
                                LOG("Closing local connection due to server FRAME_CLOSE");
                                epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                                close(local_fd);
                                local_fd = -1;
                            }
                            // Check if this is a browser cancellation vs tunnel shutdown
                            if (len > 0 && strncmp(payload, "browser_cancelled", 17) == 0) {
                                LOG("Browser request was cancelled, ready for next request");
                                // Don't shutdown client - just reset and wait for next request
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
                                ERR("Failed to send to server: %s", strerror(errno));
                                if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
                                    LOG("Server connection lost during write, will reconnect on next epoll event");
                                    break;
                                } else {
                                    goto cleanup;
                                }
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
                                WARN("Failed to notify server of close: %s", strerror(errno));
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
                LOG("EPOLLHUP|EPOLLERR on fd=%d (EPOLLHUP=%s EPOLLERR=%s)", fd,
                    (revents & EPOLLHUP) ? "yes" : "no",
                    (revents & EPOLLERR) ? "yes" : "no");
                if (fd == server_fd) {
                    ERR("Server connection lost (EPOLL event)");
                    
                    // Remove old server fd from epoll
                    epoll_ctl(epfd, EPOLL_CTL_DEL, server_fd, NULL);
                    close(server_fd);
                    
                    // Close any existing local connection
                    if (local_fd > 0) {
                        LOG("Closing local connection during reconnection");
                        epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                        close(local_fd);
                        local_fd = -1;
                    }
                    
                    // Attempt to reconnect
                    server_fd = reconnect_to_server(server_ip, server_port, tunnel_id, epfd);
                    if (server_fd < 0) {
                        ERR("Failed to reconnect to server, shutting down");
                        goto cleanup;
                    }
                } else if (fd == local_fd) {
                    LOG("Local connection closed");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                    close(local_fd);
                    local_fd = -1;
                }
            }
        }
    }

cleanup:
    LOG("Cleanup: closing connections");
    LOG("Closing server_fd=%d", server_fd);
    close(server_fd);
    if (local_fd > 0) {
        LOG("Closing local_fd=%d", local_fd);
        close(local_fd);
    }
    LOG("Closing epfd=%d", epfd);
    close(epfd);
    LOG("Shutdown complete");
    return 0;
}