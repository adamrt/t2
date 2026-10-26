// TCP connections for Terrarium, made with the host's sockets. Everything is
// non-blocking: a receive with nothing waiting returns 0, and the device
// raises an interrupt when a connection becomes readable.

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
#include <sys/socket.h>
#include <unistd.h>

#define FAILED  0xffffffffu
#define NOT_YET 0xfffffffeu

void
net_init(Net *n)
{
    memset(n, 0, sizeof(*n));
    for (int i = 0; i < NET_CONNECTIONS; i++)
        n->fds[i] = -1;
    signal(SIGPIPE, SIG_IGN);
}

static uint32_t
add(Net *n, int fd)
{
    for (uint32_t i = 0; i < NET_CONNECTIONS; i++) {
        if (n->fds[i] < 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            n->fds[i] = fd;
            n->ready[i] = false;
            return i;
        }
    }
    close(fd);
    return FAILED;
}

static uint32_t
connect_to(Net *n, Machine *m)
{
    char host[256], port[16];
    if (n->host > RAM_SIZE || n->host_length >= sizeof(host) || n->host_length > RAM_SIZE - n->host)
        return FAILED;
    memcpy(host, m->ram + n->host, n->host_length);
    host[n->host_length] = '\0';
    snprintf(port, sizeof(port), "%u", n->port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *found;
    if (getaddrinfo(host, port, &hints, &found) != 0)
        return FAILED;
    int fd = -1;
    for (struct addrinfo *a = found; a != NULL && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(found);
    return fd < 0 ? FAILED : add(n, fd);
}

// Listening only happens on the host's loopback address.
static uint32_t
listen_on(Net *n)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return FAILED;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in at = { .sin_family = AF_INET, .sin_port = htons((uint16_t)n->port), .sin_addr.s_addr = htonl(0x7f000001) }; // 127.0.0.1
    if (bind(fd, (struct sockaddr *)&at, sizeof(at)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return FAILED;
    }
    return add(n, fd);
}

static uint8_t *
buffer(Net *n, Machine *m)
{
    if (n->address > RAM_SIZE || n->length > RAM_SIZE - n->address)
        return NULL;
    return m->ram + n->address;
}

static uint32_t
command(Net *n, Machine *m, uint32_t c)
{
    if (c == NET_CONNECT)
        return connect_to(n, m);
    if (c == NET_LISTEN)
        return listen_on(n);
    if (n->id >= NET_CONNECTIONS || n->fds[n->id] < 0)
        return FAILED;
    int fd = n->fds[n->id];
    n->ready[n->id] = false;
    switch (c) {
    case NET_ACCEPT: {
        int client = accept(fd, NULL, NULL);
        if (client < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? NOT_YET : FAILED;
        return add(n, client);
    }
    case NET_SEND: {
        uint8_t *p = buffer(n, m);
        if (p == NULL)
            return FAILED;
        ssize_t sent = send(fd, p, n->length, 0);
        if (sent < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : FAILED;
        return (uint32_t)sent;
    }
    case NET_RECEIVE: {
        uint8_t *p = buffer(n, m);
        if (p == NULL)
            return FAILED;
        ssize_t got = recv(fd, p, n->length, 0);
        if (got < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? NOT_YET : FAILED;
        return got == 0 ? FAILED : (uint32_t)got;
    }
    case NET_CLOSE:
        close(fd);
        n->fds[n->id] = -1;
        return 0;
    }
    return FAILED;
}

bool
net_read(Net *n, uint32_t port, uint32_t *value)
{
    if (port != PORT_NET_RESULT)
        return false;
    *value = n->result;
    return true;
}

bool
net_write(Net *n, Machine *m, uint32_t port, uint32_t value)
{
    switch (port) {
    case PORT_NET_HOST:        n->host = value; return true;
    case PORT_NET_HOST_LENGTH: n->host_length = value; return true;
    case PORT_NET_PORT:        n->port = value; return true;
    case PORT_NET_ID:          n->id = value; return true;
    case PORT_NET_ADDRESS:     n->address = value; return true;
    case PORT_NET_LENGTH:      n->length = value; return true;
    case PORT_NET_COMMAND:     n->result = command(n, m, value); return true;
    }
    return false;
}

bool
net_active(Net *n)
{
    for (int i = 0; i < NET_CONNECTIONS; i++) {
        if (n->fds[i] >= 0)
            return true;
    }
    return false;
}

bool
net_poll(Net *n)
{
    struct pollfd fds[NET_CONNECTIONS];
    int count = 0;
    int ids[NET_CONNECTIONS];
    for (int i = 0; i < NET_CONNECTIONS; i++) {
        if (n->fds[i] >= 0 && !n->ready[i]) {
            fds[count] = (struct pollfd){ .fd = n->fds[i], .events = POLLIN };
            ids[count++] = i;
        }
    }
    if (count == 0 || poll(fds, (nfds_t)count, 0) <= 0)
        return false;
    bool any = false;
    for (int i = 0; i < count; i++) {
        if (fds[i].revents != 0) {
            n->ready[ids[i]] = true;
            any = true;
        }
    }
    return any;
}
