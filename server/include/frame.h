#ifndef RIFT_FRAME_H
#define RIFT_FRAME_H

#include <stdint.h>

#define FRAME_MAGIC 0x52494654
#define FRAME_MAX_PAYLOAD 256

typedef enum {
    FRAME_REGISTER_TUNNEL = 1,
    FRAME_CONNECT_REQUEST = 2,
    FRAME_ERROR           = 3
} frame_type_t;

typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t length;
} __attribute__((packed)) frame_header_t;

int frame_read(int fd, frame_type_t *type, char *payload, uint16_t *len);

#endif