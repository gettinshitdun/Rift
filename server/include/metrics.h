//
// Created by kanishak on 1/5/26.
//

#ifndef RIFT_METRICS_H
#define RIFT_METRICS_H

void metrics_init(void);
void metrics_inc_total_connections(void);
int metrics_total_connections(void);
long metrics_uptime(void);

#endif //RIFT_METRICS_H