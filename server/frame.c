#include <arpa/inet.h> // Required for ntohl and ntohs
#include "include/frame.h"
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

static int read_full(int fd, void *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char*)buf + off, len - off);
        if (n == 0) return -1; // Connection closed
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1; // Real error (like EAGAIN)
        }
        off += n;
    }
    return 0;
}

int frame_read(int fd, frame_type_t *type, char *payload, uint16_t *len) {
    frame_header_t hdr;

    if (read_full(fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    // CONVERT FROM NETWORK BYTE ORDER TO HOST BYTE ORDER
    uint32_t magic = ntohl(hdr.magic);
    uint16_t h_type = ntohs(hdr.type);
    uint16_t h_len  = ntohs(hdr.length);

    if (magic != FRAME_MAGIC) {
        printf("[frame] Magic mismatch: got 0x%08x, expected 0x%08x\n", magic, FRAME_MAGIC);
        return -1;
    }

    if (h_len > FRAME_MAX_PAYLOAD) {
        printf("[frame] Payload too large: %d\n", h_len);
        return -1;
    }

    if (read_full(fd, payload, h_len) < 0)
        return -1;

    payload[h_len] = '\0';

    *type = (frame_type_t)h_type;
    *len  = h_len;
    return 0;
}