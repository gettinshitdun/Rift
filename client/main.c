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
#include <signal.h>

#include "../server/include/frame.h"

#define MAX_EVENTS 10
#define DEFAULT_SERVER_IP "43.205.120.186"
#define DEFAULT_SERVER_PORT 7000

typedef struct {
    char buf[FRAME_MAX_PAYLOAD + 16];
    size_t len;
} frame_buffer_t;

static volatile int should_exit = 0;

static void signal_handler(int signum) {
    if (signum == SIGINT) {
        should_exit = 1;
    }
}

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

    // Ignore SIGPIPE to prevent crash when writing to closed connections
    signal(SIGPIPE, SIG_IGN);

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
    printf("-------------------\n");
    printf("🔄 Tunnel active. Press Ctrl+C to stop.\n\n");

    // Register Ctrl+C handler for graceful shutdown
    signal(SIGINT, signal_handler);

    // Retry loop with exponential backoff
    int retry_count = 0;
    const int max_backoff_ms = 30000;  // 30 seconds max

    while (!should_exit) {
        int server_fd = tcp_connect(server_ip, server_port);
        if (server_fd < 0) {
            int backoff_ms = (1 << retry_count) * 1000;  // Exponential: 1s, 2s, 4s, 8s, ...
            if (backoff_ms > max_backoff_ms) backoff_ms = max_backoff_ms;
            
            printf("[%d] Connection failed, retrying in %d ms...\n", ++retry_count, backoff_ms);
            usleep(backoff_ms * 1000);
            continue;
        }

        if (frame_write(server_fd, FRAME_REGISTER_TUNNEL, tunnel_id, (uint32_t)strlen(tunnel_id)) < 0) {
            fprintf(stderr, "[!] Failed to send registration frame\n");
            close(server_fd);
            int backoff_ms = (1 << retry_count) * 1000;
            if (backoff_ms > max_backoff_ms) backoff_ms = max_backoff_ms;
            printf("[%d] Registration failed, retrying in %d ms...\n", ++retry_count, backoff_ms);
            usleep(backoff_ms * 1000);
            continue;
        }

        // Reset retry counter on successful connection
        retry_count = 0;
        printf("[✓] Tunnel connected, relay active\n\n");

        int local_fd = -1;
        int epfd = epoll_create1(0);
        if (epfd < 0) {
            perror("epoll_create1");
            close(server_fd);
            continue;
        }

        struct epoll_event ev, events[MAX_EVENTS];

        ev.events = EPOLLIN;
        ev.data.fd = server_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev);

        int connection_active = 1;
        while (connection_active && !should_exit) {
            int nfds = epoll_wait(epfd, events, MAX_EVENTS, 1000);  // 1 second timeout to check should_exit
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
                            if (errno == ECONNRESET || errno == EPIPE) {
                                printf("[*] Server connection lost, will reconnect...\n");
                                connection_active = 0;
                                break;
                            }
                            // Log but continue on transient errors
                            fprintf(stderr, "[*] Frame read error (continuing): %s\n", strerror(errno));
                            break;
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
                                    if (errno == EPIPE || errno == ECONNRESET) {
                                        // Connection was closed, don't spam errors
                                        if (frame_write(server_fd, FRAME_CLOSE, NULL, 0) < 0) {
                                            // Ignore errors on FRAME_CLOSE
                                        }
                                        epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                                        close(local_fd);
                                        local_fd = -1;
                                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                        fprintf(stderr, "[*] Write error (continuing): %s\n", strerror(errno));
                                    }
                                } else if (written != (ssize_t)len) {
                                    fprintf(stderr, "[*] Partial write: %ld/%u bytes\n", written, len);
                                }
                            } else {
                                // Silently drop bytes when no connection (normal during refresh)
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
                                connection_active = 0; break;
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
                    fprintf(stderr, "[*] Server hangup detected\n");
                    // Don't exit on hangup - the connection may recover
                    // Just log and continue
                } else if (fd == local_fd) {
                    // Local connection closed (browser cancelled request)
                    epoll_ctl(epfd, EPOLL_CTL_DEL, local_fd, NULL);
                    close(local_fd);
                    local_fd = -1;
                }
            }
        }

        // Connection loop ended, cleanup and retry if not exiting
        close(server_fd);
        if (local_fd > 0) close(local_fd);
        close(epfd);

        if (should_exit) {
            break;  // Exit the retry loop if Ctrl+C was pressed
        }

        // Wait before retrying
        if (!should_exit) {
            int backoff_ms = (1 << retry_count) * 1000;
            if (backoff_ms > max_backoff_ms) backoff_ms = max_backoff_ms;
            printf("[%d] Connection lost, retrying in %d ms...\n", ++retry_count, backoff_ms);
            usleep(backoff_ms * 1000);
        }
    }

    printf("\n[✓] Tunnel stopped (Ctrl+C)\n");
    return 0;
}
}
