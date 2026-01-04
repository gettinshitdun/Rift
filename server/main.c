//
// Created by kanishak on 1/3/26.
//

#include <stdio.h>
#include <stdlib.h>

extern int epoll_server_main();

int main() {
    printf("Starting Rift server...\n");
    return epoll_server_main();
}
