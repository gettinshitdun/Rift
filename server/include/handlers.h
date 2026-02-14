#ifndef HANDLERS_H
#define HANDLERS_H

#include <sys/types.h>

int handle_http_request(int fd, const char *peek_buf);
int handle_rift_frame(int fd);
void send_http_error(int fd, const char *status, const char *msg);

#endif
