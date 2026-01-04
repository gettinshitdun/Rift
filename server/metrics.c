//
// Created by kanishak on 1/5/26.
//

#include <time.h>

static time_t start_time;
static int total_connections = 0;

void metrics_init() {
    start_time = time(NULL);
}

void metrics_inc_total_connections() {
    total_connections++;
}

int metrics_total_connections() {
    return total_connections;
}

long metrics_uptime() {
    return time(NULL) - start_time;
}