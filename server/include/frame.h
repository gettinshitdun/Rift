#ifndef RIFT_FRAME_H
#define RIFT_FRAME_H

#include <stdint.h>
#include <arpa/inet.h> // For htons/ntohs

#define FRAME_MAGIC 0x52494654
#define FRAME_VERSION 2
#define FRAME_MAX_PAYLOAD 16384

typedef enum {
    FRAME_REGISTER_TUNNEL = 1,
    FRAME_CONNECT_REQUEST = 2,
    FRAME_DATA            = 3,
    FRAME_ERROR           = 4,
    FRAME_CLOSE           = 5,
    FRAME_ACK             = 6
} frame_type_t;

/*
 * V2 wire format (16 bytes):
 *   magic      (4)  0x52494654
 *   version    (1)  2
 *   reserved   (1)  0
 *   type       (2)  frame_type_t
 *   length     (4)  payload length
 *   stream_id  (4)  per-browser stream identifier (0 = control)
 */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  reserved;
    uint16_t type;
    uint32_t length;
    uint32_t stream_id;
} __attribute__((packed)) frame_header_t;

int frame_read(int fd, frame_type_t *type, char *payload, uint32_t *len, uint32_t *stream_id);
int frame_write(int fd, frame_type_t type, const char *payload, uint32_t len, uint32_t stream_id);

#endif