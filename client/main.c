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

#define MAX_EVENTS 64
#define DEFAULT_SERVER_IP "43.205.120.186"
#define DEFAULT_SERVER_PORT 7000
#define MAX_RECONNECT_ATTEMPTS 5
#define RECONNECT_DELAY_BASE 2  // seconds

/* Maximum concurrent streams (matches server) */
#define MAX_STREAMS 128

// Global verbose flag
static int verbose = 0;

#define LOG(fmt, ...) do { if (verbose) fprintf(stderr, "[LOG] " fmt "\n", ##__VA_ARGS__); } while(0)
#define WARN(fmt, ...) fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

/* ---- Stream map: stream_id <-> local_fd ---- */
typedef struct {
    uint32_t stream_id;
    int      local_fd;
} stream_map_entry_t;

static stream_map_entry_t stream_map[MAX_STREAMS];
static int stream_count = 0;

static int stream_add(uint32_t sid, int local_fd) {
    if (stream_count >= MAX_STREAMS) return -1;
    stream_map[stream_count].stream_id = sid;
    stream_map[stream_count].local_fd = local_fd;
    stream_count++;
    return 0;
}

static int stream_find_fd(uint32_t sid) {
    for (int i = 0; i < stream_count; i++)
        if (stream_map[i].stream_id == sid) return stream_map[i].local_fd;
    return -1;
}

static uint32_t stream_find_sid(int local_fd) {
    for (int i = 0; i < stream_count; i++)
        if (stream_map[i].local_fd == local_fd) return stream_map[i].stream_id;
    return 0;
}

static void stream_remove_by_sid(uint32_t sid) {
    for (int i = 0; i < stream_count; i++) {
        if (stream_map[i].stream_id == sid) {
            stream_map[i] = stream_map[stream_count - 1];
            stream_count--;
            return;
        }
    }
}

static void stream_remove_by_fd(int local_fd) {
    for (int i = 0; i < stream_count; i++) {
        if (stream_map[i].local_fd == local_fd) {
            stream_map[i] = stream_map[stream_count - 1];
            stream_count--;
            return;
        }
    }
}

static void stream_close_all(int epfd) {
    for (int i = 0; i < stream_count; i++) {
        int lfd = stream_map[i].local_fd;
        epoll_ctl(epfd, EPOLL_CTL_DEL, lfd, NULL);
        close(lfd);
    }
    stream_count = 0;
}

// Reliable write: retries on partial writes and EAGAIN
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    int retries = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) { off += n; retries = 0; }
        else if (n == 0) { return -1; }
        else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++retries > 50) return -1;
                usleep(1000);
                continue;
            }
            return -1;
        }
    }
    return 0;
}

typedef struct {
    char buf[FRAME_MAX_PAYLOAD + 20];  /* 16-byte header + payload */
    size_t len;
} frame_buffer_t;

static frame_buffer_t frame_buf = {0};

// Check if local service is running on the specified port
int check_local_service(int port) {
    int test_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (test_fd < 0) {
        return -1;
    }
    
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // Try to connect with a short timeout
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(test_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(test_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    int result = connect(test_fd, (struct sockaddr*)&addr, sizeof(addr));
    close(test_fd);
    
    if (result == 0) {
        // Connected successfully - service is running
        return 0;
    } else {
        // Connection failed - check why
        if (errno == ECONNREFUSED) {
            // Port is not open - no service running
            return -1;
        } else if (errno == ETIMEDOUT) {
            // Timeout - probably no service
            return -1;
        }
        // Other errors - assume no service
        return -1;
    }
}

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

int frame_read_buffered(int fd, frame_type_t *type, char *payload, uint32_t *len, uint32_t *stream_id) {
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
    uint32_t h_sid = ntohl(hdr->stream_id);

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
    *stream_id = h_sid;
    if (h_len > 0) {
        memcpy(payload, frame_buf.buf + sizeof(frame_header_t), h_len);
    }

    // Remove the processed frame from the buffer
    memmove(frame_buf.buf, frame_buf.buf + frame_total, frame_buf.len - frame_total);
    frame_buf.len -= frame_total;

    return 0;
}

int reconnect_to_server(const char *server_ip, int server_port, const char *tunnel_id, int epfd) {
    // Close all active local streams before reconnecting
    stream_close_all(epfd);

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
        
        // Send registration frame (stream_id=0 for control frames)
        LOG("Re-registering tunnel: %s", tunnel_id);
        if (frame_write(server_fd, FRAME_REGISTER_TUNNEL, tunnel_id, (uint32_t)strlen(tunnel_id), 0) < 0) {
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
    // Parse arguments - support verbose flag
    if (argc < 3) {
        fprintf(stderr, "Usage: %s [--verbose] expose <port>\n", argv[0]);
        fprintf(stderr, "  --verbose    Enable debug logging\n");
        return 1;
    }
    
    int arg_idx = 1;
    if (argc >= 4 && strcmp(argv[1], "--verbose") == 0) {
        verbose = 1;
        arg_idx = 2;
    }
    
    if (strcmp(argv[arg_idx], "expose") != 0) {
        fprintf(stderr, "Usage: %s [--verbose] expose <port>\n", argv[0]);
        return 1;
    }

    int local_port = atoi(argv[arg_idx + 1]);
    if (local_port <= 0 || local_port > 65535) {
        fprintf(stderr, "Error: Invalid port %d\n", local_port);
        return 1;
    }

    // Check if local service is running on the port
    printf("Checking local service on port %d...\n", local_port);
    if (check_local_service(local_port) < 0) {
        fprintf(stderr, "Error: No service found running on port %d\n", local_port);
        fprintf(stderr, "Please start your local service first, then run:\n");
        fprintf(stderr, "  %s expose %d\n", argv[0], local_port);
        return 1;
    }
    printf("✓ Local service detected on port %d\n", local_port);

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

    printf("\n--- RIFT CLIENT v2 (stream multiplex) ---\n");
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

    // Register tunnel (stream_id=0 for control frames)
    LOG("Sending registration frame for tunnel: %s", tunnel_id);
    if (frame_write(server_fd, FRAME_REGISTER_TUNNEL, tunnel_id, (uint32_t)strlen(tunnel_id), 0) < 0) {
        ERR("Failed to send registration frame");
        close(server_fd);
        return 1;
    }
    LOG("Registration successful");

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

    LOG("Event loop started (stream multiplexing mode)");
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            ERR("epoll_wait failed: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            uint32_t revents = events[i].events;
            int fd = events[i].data.fd;

            if (revents & EPOLLIN) {
                if (fd == server_fd) {
                    // ── Read frames from the tunnel server ──
                    while (1) {
                        frame_type_t type;
                        char payload[FRAME_MAX_PAYLOAD];
                        uint32_t len;
                        uint32_t stream_id;

                        int res = frame_read_buffered(server_fd, &type, payload, &len, &stream_id);
                        if (res < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPROTO || errno == EBADMSG) {
                                break;  // No more complete frames
                            }
                            if (errno == ECONNRESET || errno == EPIPE) {
                                ERR("Server connection lost: %s", strerror(errno));
                                epoll_ctl(epfd, EPOLL_CTL_DEL, server_fd, NULL);
                                close(server_fd);

                                server_fd = reconnect_to_server(server_ip, server_port, tunnel_id, epfd);
                                if (server_fd < 0) {
                                    ERR("Failed to reconnect, shutting down");
                                    goto cleanup;
                                }
                                break;
                            }
                            WARN("Frame read error: %s (continuing)", strerror(errno));
                            break;
                        }

                        LOG("Frame: type=%d len=%u stream=%u", type, len, stream_id);

                        if (type == FRAME_CONNECT_REQUEST) {
                            // ── New browser request → open a fresh local connection ──
                            LOG("CONNECT_REQUEST stream=%u", stream_id);

                            int lfd = tcp_connect("127.0.0.1", local_port);
                            if (lfd < 0) {
                                ERR("Cannot connect to local service for stream %u: %s",
                                    stream_id, strerror(errno));
                                // Notify server this stream failed
                                frame_write(server_fd, FRAME_CLOSE, "local_failed", 12, stream_id);
                                continue;
                            }

                            if (stream_add(stream_id, lfd) < 0) {
                                ERR("Stream map full, rejecting stream %u", stream_id);
                                close(lfd);
                                frame_write(server_fd, FRAME_CLOSE, "stream_limit", 12, stream_id);
                                continue;
                            }

                            struct epoll_event lev = {.events = EPOLLIN, .data.fd = lfd};
                            epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev);
                            LOG("Stream %u → local fd=%d", stream_id, lfd);
                        }
                        else if (type == FRAME_DATA) {
                            // ── Forward data to the matching local connection ──
                            int lfd = stream_find_fd(stream_id);
                            if (lfd < 0) {
                                WARN("DATA for unknown stream %u, dropping %u bytes", stream_id, len);
                                continue;
                            }
                            if (write_all(lfd, payload, len) < 0) {
                                ERR("Write to local fd=%d (stream %u) failed: %s",
                                    lfd, stream_id, strerror(errno));
                                epoll_ctl(epfd, EPOLL_CTL_DEL, lfd, NULL);
                                close(lfd);
                                stream_remove_by_sid(stream_id);
                                frame_write(server_fd, FRAME_CLOSE, "write_error", 11, stream_id);
                            }
                        }
                        else if (type == FRAME_CLOSE) {
                            // ── Server closed one stream (browser disconnected) ──
                            LOG("CLOSE stream=%u", stream_id);
                            int lfd = stream_find_fd(stream_id);
                            if (lfd >= 0) {
                                epoll_ctl(epfd, EPOLL_CTL_DEL, lfd, NULL);
                                close(lfd);
                            }
                            stream_remove_by_sid(stream_id);
                        }
                        else {
                            WARN("Unknown frame type %d on stream %u", type, stream_id);
                        }
                    }
                }
                else {
                    // ── Data from a local service connection → send to server ──
                    uint32_t sid = stream_find_sid(fd);
                    if (sid == 0) {
                        // Stale fd – not in stream map anymore
                        WARN("Data on unknown local fd=%d, removing", fd);
                        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        close(fd);
                        continue;
                    }

                    char buffer[FRAME_MAX_PAYLOAD];
                    while (1) {
                        ssize_t n = read(fd, buffer, sizeof(buffer));
                        if (n > 0) {
                            if (frame_write(server_fd, FRAME_DATA, buffer, (uint32_t)n, sid) < 0) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                                    LOG("Server backpressure on stream %u, pausing", sid);
                                    break;
                                } else if (errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN) {
                                    ERR("Server connection lost during write");
                                    break;
                                } else {
                                    ERR("Send to server failed: %s", strerror(errno));
                                    goto cleanup;
                                }
                            }
                        } else if (n < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                break;  // No more data right now
                            }
                            ERR("Read error from local fd=%d (stream %u): %s",
                                fd, sid, strerror(errno));
                            break;
                        } else {
                            // Local connection closed – tell server to close the stream
                            LOG("Local fd=%d closed (stream %u)", fd, sid);
                            frame_write(server_fd, FRAME_CLOSE, NULL, 0, sid);
                            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                            close(fd);
                            stream_remove_by_fd(fd);
                            break;
                        }
                    }
                }
            }

            if (revents & (EPOLLHUP | EPOLLERR)) {
                if (fd == server_fd) {
                    ERR("Server connection lost (EPOLL event)");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, server_fd, NULL);
                    close(server_fd);

                    server_fd = reconnect_to_server(server_ip, server_port, tunnel_id, epfd);
                    if (server_fd < 0) {
                        ERR("Failed to reconnect, shutting down");
                        goto cleanup;
                    }
                } else {
                    // A local connection hung up
                    uint32_t sid = stream_find_sid(fd);
                    LOG("Local fd=%d HUP/ERR (stream %u)", fd, sid);
                    if (sid != 0) {
                        frame_write(server_fd, FRAME_CLOSE, NULL, 0, sid);
                        stream_remove_by_fd(fd);
                    }
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close(fd);
                }
            }
        }
    }

cleanup:
    LOG("Cleanup: closing all streams and connections");
    stream_close_all(epfd);
    close(server_fd);
    close(epfd);
    LOG("Shutdown complete");
    return 0;
}