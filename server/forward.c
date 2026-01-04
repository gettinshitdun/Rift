//
// forward.c
// Created by kanishak on 1/4/26.
//
#define _GNU_SOURCE

#include "include/forward.h"
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SPLICE_SIZE 65536  // max bytes per splice call

void forward_data(int src_fd, int dst_fd) {
    // Create a pipe for splice
    int pipe_fds[2];
    if (pipe(pipe_fds) < 0) {
        perror("pipe");
        return;
    }

    // Make pipe non-blocking
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(pipe_fds[i], F_GETFL, 0);
        fcntl(pipe_fds[i], F_SETFL, flags | O_NONBLOCK);
    }

    while (1) {
        // Splice from src_fd -> pipe
        ssize_t n = splice(src_fd, NULL, pipe_fds[1], NULL, SPLICE_SIZE, SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
        if (n > 0) {
            ssize_t total_written = 0;
            while (total_written < n) {
                // Splice from pipe -> dst_fd
                ssize_t w = splice(pipe_fds[0], NULL, dst_fd, NULL, n - total_written,
                                   SPLICE_F_MOVE | SPLICE_F_NONBLOCK);
                if (w > 0) {
                    total_written += w;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue; // try again later
                } else {
                    perror("splice write");
                    goto cleanup;
                }
            }
        } else if (n == 0) {
            // EOF — source closed
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more data for now
                break;
            } else {
                perror("splice read");
                break;
            }
        }
    }

    cleanup:
        close(pipe_fds[0]);
        close(pipe_fds[1]);
}
