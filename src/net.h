#pragma once

#include "machine.h"

// The network device: TCP connections made with the host's sockets. See
// docs/devices.md.

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
    NET_AVAILABLE = 7,
};

#define NET_CONNECTIONS 16

typedef struct {
    int fd;     // -1 when unused.
    bool ready; // Already reported; rearmed by the next command.
    bool connecting;
    bool broken;
    bool want_write;
    bool listening;
} NetConnection;

typedef struct {
    NetConnection connections[NET_CONNECTIONS];
    uint32_t host_address;
    uint32_t host_length;
    uint32_t port;
    uint32_t id;
    uint32_t address;
    uint32_t length;
    uint32_t result;
} Net;

void net_init(Net *net);
void net_close(Net *net);
bool net_read(Net *net, uint32_t port, uint32_t *value);
bool net_write(Net *net, Machine *machine, uint32_t port, uint32_t value);
// Returns true when a connection has become readable or writable since the
// last call.
bool net_poll(Net *net);
// Whether any connection is open, so the machine should keep checking.
bool net_active(const Net *net);
