#pragma once

#include "machine.h"

// The network device: TCP connections made with the host's sockets. See
// spec/devices.md.

enum {
    PORT_NET_HOST = 0x70,
    PORT_NET_HOST_LENGTH = 0x71,
    PORT_NET_PORT = 0x72,
    PORT_NET_ID = 0x73,
    PORT_NET_ADDRESS = 0x74,
    PORT_NET_LENGTH = 0x75,
    PORT_NET_COMMAND = 0x76,
    PORT_NET_RESULT = 0x77,
};

enum {
    NET_CONNECT = 1,
    NET_LISTEN = 2,
    NET_ACCEPT = 3,
    NET_SEND = 4,
    NET_RECEIVE = 5,
    NET_CLOSE = 6,
};

#define NET_CONNECTIONS 16

typedef struct {
    int fds[NET_CONNECTIONS];   // -1 when unused
    bool ready[NET_CONNECTIONS];
    uint32_t host, host_length, port, id, address, length, result;
} Net;

void net_init(Net *n);
bool net_read(Net *n, uint32_t port, uint32_t *value);
bool net_write(Net *n, Machine *m, uint32_t port, uint32_t value);
// Returns true when a connection has become readable since the last call.
bool net_poll(Net *n);
// Whether any connection is open, so the machine should keep checking.
bool net_active(Net *n);
