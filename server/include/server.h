//
// Created by kanishak on 1/4/26.
//

#pragma once

#ifndef RIFT_SERVER_H
#define RIFT_SERVER_H

// Start the Rift server
// tunnel_port: port for tunnel clients
// public_port: port for public users
// returns 0 on clean exit, -1 on error
int server_run(int tunnel_port, int public_port);


#endif //RIFT_SERVER_H