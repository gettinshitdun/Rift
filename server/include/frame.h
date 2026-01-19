#ifndef RIFT_FRAME_H
#define RIFT_FRAME_H

#include <stdint.h>
#include <arpa/inet.h> // For htons/ntohs

#define FRAME_MAGIC 0x52494654
// Increase this! Standard Ethernet MTU is 1500, but 16KB is safer for HTTP headers.
#define FRAME_MAX_PAYLOAD 16384 

typedef enum {
    FRAME_REGISTER_TUNNEL = 1,
    FRAME_CONNECT_REQUEST = 2,
    FRAME_DATA             = 3, // <--- Add this!
    FRAME_ERROR            = 4
} frame_type_t;

typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t length;
} __attribute__((packed)) frame_header_t;

int frame_read(int fd, frame_type_t *type, char *payload, uint16_t *len);
int frame_write(int fd, frame_type_t type, const char *payload, uint16_t len);

#endif