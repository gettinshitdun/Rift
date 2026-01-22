#ifndef RIFT_FRAME_H
#define RIFT_FRAME_H

#include <stdint.h>
#include <arpa/inet.h> // For htons/ntohs

#define FRAME_MAGIC 0x52494654
#define FRAME_VERSION 1
#define FRAME_MAX_PAYLOAD 16384 

typedef enum {
    FRAME_REGISTER_TUNNEL = 1,
    FRAME_CONNECT_REQUEST = 2,
    FRAME_DATA            = 3,
    FRAME_ERROR           = 4,
    FRAME_CLOSE           = 5,
    FRAME_ACK             = 6
} frame_type_t;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  reserved;
    uint16_t type;
    uint32_t length;
} __attribute__((packed)) frame_header_t;

int frame_read(int fd, frame_type_t *type, char *payload, uint32_t *len);
int frame_write(int fd, frame_type_t type, const char *payload, uint32_t len);

#endif