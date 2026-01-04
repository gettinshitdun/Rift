//
// Created by kanishak on 1/4/26.
//

#include "include/forward.h"
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#define BUF_SIZE 4096

void forward_data(int src_fd, int dst_fd) {
    char buf[BUF_SIZE];
    ssize_t n;

    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(dst_fd, buf + written, n - written);
            if (w <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue; // non-blocking, try again
                perror("write");
                return;
            }
            written += w;
        }
    }

    if (n == 0) {
        // src_fd closed
        // the caller (epoll loop) should handle cleanup
        // do not close here; leave connection_close() to epoll
    } else if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("read");
    }
}
