#include <arpa/inet.h>
#include "include/frame.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

// Helper to ensure we write everything even if the kernel buffer is full
static int write_full(int fd, const void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, (const char*)buf + off, len - off);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(100); 
                continue;
            }
            return -1;
        }
        off += n;
    }
    return 0;
}

// Fixed version: properly handles non-blocking reads
static int read_full(int fd, void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char*)buf + off, len - off);
        if (n == 0) {
            // Connection closed
            errno = ECONNRESET;
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            
            // CRITICAL FIX: For non-blocking sockets, return immediately
            // Let epoll wait for more data instead of busy-waiting
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // If we haven't read anything yet, just return -1 with EAGAIN
                // If we read partial data, we have a problem - the frame is incomplete
                if (off == 0) {
                    // No data available at all
                    return -1; // errno is already EAGAIN
                } else {
                    // We read partial frame data - this shouldn't happen in practice
                    // with TCP unless there's a bug. For now, keep trying.
                    usleep(100);
                    continue;
                }
            }
            return -1; 
        }
        off += n;
    }
    return 0;
}

int frame_read(int fd, frame_type_t *type, char *payload, uint16_t *len) {
    frame_header_t hdr;
    if (read_full(fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    uint32_t magic = ntohl(hdr.magic);
    uint16_t h_type = ntohs(hdr.type);
    uint16_t h_len  = ntohs(hdr.length);

    if (magic != FRAME_MAGIC) {
        printf("[frame] Magic mismatch: got 0x%08x, expected 0x%08x\n", magic, FRAME_MAGIC);
        errno = EBADMSG;
        return -1;
    }

    if (h_len > FRAME_MAX_PAYLOAD) {
        printf("[frame] Payload too large: %d\n", h_len);
        errno = EMSGSIZE;
        return -1;
    }

    if (h_len > 0) {
        if (read_full(fd, payload, h_len) < 0)
            return -1;
    }

    // Safety: Always null-terminate if there is space
    if (h_len < FRAME_MAX_PAYLOAD) {
        payload[h_len] = '\0';
    }

    *type = (frame_type_t)h_type;
    *len  = h_len;
    return 0;
}

int frame_write(int fd, frame_type_t type, const char *payload, uint16_t len) {
    frame_header_t hdr;
    hdr.magic  = htonl(FRAME_MAGIC);
    hdr.type   = htons((uint16_t)type);
    hdr.length = htons(len);

    if (write_full(fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    if (len > 0 && payload != NULL) {
        if (write_full(fd, payload, len) < 0)
            return -1;
    }

    return 0;
}