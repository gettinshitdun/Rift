#include <arpa/inet.h>
#include "include/frame.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <poll.h>

// Helper to ensure we write everything even if the kernel buffer is full.
// Uses poll(POLLOUT) with a total timeout of 30 seconds to handle
// backpressure during heavy transfers (video, large PDFs, etc.).
static int write_full(int fd, const void *buf, size_t len) {
    size_t off = 0;
    const int POLL_INTERVAL_MS = 5000;  // 5s per poll call
    const int MAX_POLLS = 6;            // 30s total timeout

    while (off < len) {
        ssize_t n = write(fd, (const char*)buf + off, len - off);
        if (n > 0) {
            off += n;
        } else if (n == 0) {
            return -1;  // Socket closed
        } else {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Loop poll() to handle extended backpressure
                int ready = 0;
                for (int attempt = 0; attempt < MAX_POLLS; attempt++) {
                    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                    int pr = poll(&pfd, 1, POLL_INTERVAL_MS);
                    if (pr > 0) {
                        if (pfd.revents & (POLLERR | POLLHUP)) return -1;
                        ready = 1;
                        break;
                    }
                    if (pr < 0 && errno != EINTR) return -1;
                    // pr == 0: timeout, try again
                }
                if (!ready) {
                    errno = ETIMEDOUT;
                    return -1;
                }
                continue;
            }
            return -1;  // Real error
        }
    }
    return 0;
}

// Handles non-blocking reads with retry logic
static int read_full(int fd, void *buf, size_t len) {
    size_t off = 0;
    int retries = 0;
    const int MAX_RETRIES = 30;

    while (off < len) {
        ssize_t n = read(fd, (char*)buf + off, len - off);
        if (n > 0) {
            off += n;
            retries = 0;  // Reset on data received
        } else if (n == 0) {
            // Connection closed by peer
            if (off == 0) {
                errno = ECONNRESET;
            }
            return -1;
        } else {
            if (errno == EINTR) {
                continue;  // Retry on interrupt
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Data not available yet - retry with backoff
                if (++retries < MAX_RETRIES) {
                    usleep(1000);  // 1ms wait for data to arrive
                    continue;
                }
                // Give up after max retries
                errno = ETIMEDOUT;
                return -1;
            }
            return -1;  // Real error
        }
    }
    return 0;
}

int frame_read(int fd, frame_type_t *type, char *payload, uint32_t *len, uint32_t *stream_id) {
    frame_header_t hdr;
    if (read_full(fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    uint32_t magic = ntohl(hdr.magic);
    uint8_t version = hdr.version;
    uint16_t h_type = ntohs(hdr.type);
    uint32_t h_len = ntohl(hdr.length);
    uint32_t h_sid = ntohl(hdr.stream_id);

    if (magic != FRAME_MAGIC) {
        fprintf(stderr, "[frame] Magic mismatch: got 0x%08x, expected 0x%08x\n", magic, FRAME_MAGIC);
        errno = EBADMSG;
        return -1;
    }

    if (version != FRAME_VERSION) {
        fprintf(stderr, "[frame] Unsupported protocol version: %d (expected %d)\n", version, FRAME_VERSION);
        errno = EPROTO;
        return -1;
    }

    if (h_len > FRAME_MAX_PAYLOAD) {
        fprintf(stderr, "[frame] Payload too large: %u (max %u)\n", h_len, FRAME_MAX_PAYLOAD);
        errno = EMSGSIZE;
        return -1;
    }

    if (h_len > 0) {
        if (read_full(fd, payload, h_len) < 0)
            return -1;
    }

    *type = (frame_type_t)h_type;
    *len = h_len;
    *stream_id = h_sid;
    return 0;
}

int frame_write(int fd, frame_type_t type, const char *payload, uint32_t len, uint32_t stream_id) {
    if (len > FRAME_MAX_PAYLOAD) {
        fprintf(stderr, "[frame] Payload too large to send: %u (max %u)\n", len, FRAME_MAX_PAYLOAD);
        errno = EMSGSIZE;
        return -1;
    }

    frame_header_t hdr;
    hdr.magic = htonl(FRAME_MAGIC);
    hdr.version = FRAME_VERSION;
    hdr.reserved = 0;
    hdr.type = htons((uint16_t)type);
    hdr.length = htonl(len);
    hdr.stream_id = htonl(stream_id);

    // Combine header + payload into a single buffer for atomic write.
    // Prevents protocol desync if connection drops between two separate writes.
    char frame_buf[sizeof(frame_header_t) + FRAME_MAX_PAYLOAD];
    memcpy(frame_buf, &hdr, sizeof(hdr));
    if (len > 0 && payload != NULL) {
        memcpy(frame_buf + sizeof(hdr), payload, len);
    }

    return write_full(fd, frame_buf, sizeof(hdr) + len);
}