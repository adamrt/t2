// TCP connections for Terrarium, made with the host's sockets. Nothing
// blocks except looking up a host name: a connect returns at once and
// finishes later, a receive with nothing waiting returns NET_NOT_READY, and the
// device raises an interrupt when a connection becomes readable, finishes
// connecting, or has room again after a send found its buffer full.

#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define NET_FAILED 0xffffffffu
#define NET_NOT_READY 0xfffffffeu

void
net_init(Net *net)
{
    memset(net, 0, sizeof(*net));
    for (int i = 0; i < NET_CONNECTIONS; i++)
        net->connections[i].fd = -1;
    signal(SIGPIPE, SIG_IGN);
}

void
net_close(Net *net)
{
    for (int i = 0; i < NET_CONNECTIONS; i++) {
        if (net->connections[i].fd >= 0)
            close(net->connections[i].fd);
        net->connections[i].fd = -1;
    }
}

static bool
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Takes ownership of fd, including when the connection table is full.
static uint32_t
add_connection(Net *net, int fd, bool connecting)
{
    if (set_nonblocking(fd)) {
        for (uint32_t i = 0; i < NET_CONNECTIONS; i++) {
            if (net->connections[i].fd < 0) {
                net->connections[i] = (NetConnection){
                    .fd = fd,
                    .connecting = connecting,
                    .want_write = connecting,
                };
                return i;
            }
        }
    }
    close(fd);
    return NET_FAILED;
}

static bool
valid_port(uint32_t port)
{
    return port > 0 && port <= 65535;
}

static uint32_t
connect_host(Net *net, Machine *machine)
{
    char host[256], port[16];
    if (net->host_address > RAM_SIZE || net->host_length >= sizeof(host) ||
        net->host_length > RAM_SIZE - net->host_address)
        return NET_FAILED;
    memcpy(host, machine->ram + net->host_address, net->host_length);
    host[net->host_length] = '\0';
    if (!valid_port(net->port))
        return NET_FAILED;
    snprintf(port, sizeof(port), "%u", net->port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *found;
    if (getaddrinfo(host, port, &hints, &found) != 0)
        return NET_FAILED;
    // The connect finishes later, so only one address gets a try: IPv4 if
    // there is one, since Terrarium's own servers listen there.
    struct addrinfo *address = found;
    for (struct addrinfo *candidate = found; candidate != NULL; candidate = candidate->ai_next) {
        if (candidate->ai_family == AF_INET) {
            address = candidate;
            break;
        }
    }
    uint32_t result = NET_FAILED;
    int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd >= 0) {
        if (!set_nonblocking(fd)) {
            close(fd);
            freeaddrinfo(found);
            return NET_FAILED;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
            result = add_connection(net, fd, false);
        else if (errno == EINPROGRESS)
            result = add_connection(net, fd, true);
        else
            close(fd);
    }
    freeaddrinfo(found);
    return result;
}

// Listening only happens on the host's loopback address.
static uint32_t
listen_on(Net *net)
{
    if (!valid_port(net->port))
        return NET_FAILED;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return NET_FAILED;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in at = { .sin_family = AF_INET,
                              .sin_port = htons((uint16_t)net->port),
                              .sin_addr.s_addr = htonl(0x7f000001) }; // 127.0.0.1
    if (bind(fd, (struct sockaddr *)&at, sizeof(at)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return NET_FAILED;
    }
    uint32_t id = add_connection(net, fd, false);
    if (id != NET_FAILED)
        net->connections[id].listening = true;
    return id;
}

// Bytes waiting to be received, or for a listener 1 if someone is waiting.
static uint32_t
bytes_available(Net *net, uint32_t id)
{
    NetConnection *connection = &net->connections[id];
    int fd = connection->fd;
    if (connection->listening) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        return poll(&p, 1, 0) > 0 ? 1 : 0;
    }
    if (connection->connecting)
        return 0;
    uint8_t byte;
    ssize_t got = recv(fd, &byte, 1, MSG_PEEK);
    if (got < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : NET_FAILED;
    if (got == 0)
        return NET_FAILED;
    int waiting = 0;
    ioctl(fd, FIONREAD, &waiting);
    return waiting > 0 ? (uint32_t)waiting : 1;
}

static uint8_t *
transfer_buffer(Net *net, Machine *machine)
{
    if (net->address > RAM_SIZE || net->length > RAM_SIZE - net->address)
        return NULL;
    return machine->ram + net->address;
}

static uint32_t
run_command(Net *net, Machine *machine, uint32_t command)
{
    if (command == NET_CONNECT)
        return connect_host(net, machine);
    if (command == NET_LISTEN)
        return listen_on(net);
    if (net->id >= NET_CONNECTIONS || net->connections[net->id].fd < 0)
        return NET_FAILED;
    uint32_t id = net->id;
    NetConnection *connection = &net->connections[id];
    int fd = connection->fd;
    connection->ready = false;
    if (command != NET_CLOSE && connection->broken)
        return NET_FAILED;
    switch (command) {
    case NET_ACCEPT: {
        int client = accept(fd, NULL, NULL);
        if (client < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? NET_NOT_READY : NET_FAILED;
        return add_connection(net, client, false);
    }
    case NET_SEND: {
        uint8_t *p = transfer_buffer(net, machine);
        if (p == NULL)
            return NET_FAILED;
        if (connection->connecting)
            return 0;
        ssize_t sent = send(fd, p, net->length, 0);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            connection->want_write = true;
            return 0;
        }
        return sent < 0 ? NET_FAILED : (uint32_t)sent;
    }
    case NET_RECEIVE: {
        uint8_t *p = transfer_buffer(net, machine);
        if (p == NULL)
            return NET_FAILED;
        if (connection->connecting)
            return NET_NOT_READY;
        ssize_t got = recv(fd, p, net->length, 0);
        if (got < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? NET_NOT_READY : NET_FAILED;
        return got == 0 ? NET_FAILED : (uint32_t)got;
    }
    case NET_AVAILABLE:
        return bytes_available(net, id);
    case NET_CLOSE:
        close(fd);
        connection->fd = -1;
        return 0;
    }
    return NET_FAILED;
}

bool
net_read(Net *net, uint32_t port, uint32_t *value)
{
    if (port != PORT_NET_RESULT)
        return false;
    *value = net->result;
    return true;
}

bool
net_write(Net *net, Machine *machine, uint32_t port, uint32_t value)
{
    switch (port) {
    case PORT_NET_HOST:
        net->host_address = value;
        return true;
    case PORT_NET_HOST_LENGTH:
        net->host_length = value;
        return true;
    case PORT_NET_PORT:
        net->port = value;
        return true;
    case PORT_NET_ID:
        net->id = value;
        return true;
    case PORT_NET_ADDRESS:
        net->address = value;
        return true;
    case PORT_NET_LENGTH:
        net->length = value;
        return true;
    case PORT_NET_COMMAND:
        net->result = run_command(net, machine, value);
        return true;
    }
    return false;
}

bool
net_active(const Net *net)
{
    for (int i = 0; i < NET_CONNECTIONS; i++) {
        if (net->connections[i].fd >= 0)
            return true;
    }
    return false;
}

bool
net_poll(Net *net)
{
    struct pollfd fds[NET_CONNECTIONS];
    int count = 0;
    int ids[NET_CONNECTIONS];
    for (int i = 0; i < NET_CONNECTIONS; i++) {
        if (net->connections[i].fd >= 0 && !net->connections[i].ready &&
            !net->connections[i].broken) {
            short events = net->connections[i].want_write ? POLLIN | POLLOUT : POLLIN;
            fds[count] = (struct pollfd){ .fd = net->connections[i].fd, .events = events };
            ids[count++] = i;
        }
    }
    if (count == 0 || poll(fds, (nfds_t)count, 0) <= 0)
        return false;
    bool any = false;
    for (int i = 0; i < count; i++) {
        if (fds[i].revents == 0)
            continue;
        NetConnection *connection = &net->connections[ids[i]];
        if (connection->connecting) {
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(connection->fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
                error = errno;
            connection->connecting = false;
            connection->broken = error != 0;
        }
        if (fds[i].revents & (POLLOUT | POLLERR | POLLHUP))
            connection->want_write = false;
        connection->ready = true;
        any = true;
    }
    return any;
}
