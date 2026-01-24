#include <arpa/inet.h>
#include "include/frame.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

// Helper to ensure we write everything even if the kernel buffer is full
static int write_full(int fd, const void *buf, size_t len) {
    size_t off = 0;
    int retries = 0;
    const int MAX_RETRIES = 10;

    while (off < len) {
        ssize_t n = write(fd, (const char*)buf + off, len - off);
        if (n > 0) {
            off += n;
            retries = 0;  // Reset on success
        } else if (n == 0) {
            return -1;  // Socket closed
        } else {
            if ((errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (++retries > MAX_RETRIES) {
                    return -1;  // Too many retries
                }
                usleep(10);  // Shorter sleep than before
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
    const int MAX_RETRIES = 100;  // Allow many retries for non-blocking sockets

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
                // Data not available yet - retry
                if (++retries < MAX_RETRIES) {
                    usleep(100);  // Wait a bit for data to arrive
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

int frame_read(int fd, frame_type_t *type, char *payload, uint32_t *len) {
    frame_header_t hdr;
    if (read_full(fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    uint32_t magic = ntohl(hdr.magic);
    uint8_t version = hdr.version;
    uint16_t h_type = ntohs(hdr.type);
    uint32_t h_len = ntohl(hdr.length);

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
    return 0;
}

int frame_write(int fd, frame_type_t type, const char *payload, uint32_t len) {
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

    if (write_full(fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    if (len > 0 && payload != NULL) {
        if (write_full(fd, payload, len) < 0)
            return -1;
    }

    return 0;
}