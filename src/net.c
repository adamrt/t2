// TCP connections for Terrarium, made with the host's sockets. Nothing
// blocks except looking up a host name: a connect returns at once and
// finishes later, a receive with nothing waiting returns NOT_YET, and the
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
add(Net *n, int fd, bool connecting)
{
    for (uint32_t i = 0; i < NET_CONNECTIONS; i++) {
        if (n->fds[i] < 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            n->fds[i] = fd;
            n->ready[i] = false;
            n->connecting[i] = connecting;
            n->broken[i] = false;
            n->want_write[i] = connecting;
            return i;
        }
    }
    close(fd);
    return FAILED;
}

static bool
valid_port(uint32_t port)
{
    return port > 0 && port <= 65535;
}

static uint32_t
connect_to(Net *n, Machine *m)
{
    char host[256], port[16];
    if (n->host > RAM_SIZE || n->host_length >= sizeof(host) || n->host_length > RAM_SIZE - n->host)
        return FAILED;
    memcpy(host, m->ram + n->host, n->host_length);
    host[n->host_length] = '\0';
    if (!valid_port(n->port))
        return FAILED;
    snprintf(port, sizeof(port), "%u", n->port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *found;
    if (getaddrinfo(host, port, &hints, &found) != 0)
        return FAILED;
    // The connect finishes later, so only one address gets a try: IPv4 if
    // there is one, since Terrarium's own servers listen there.
    struct addrinfo *a = found;
    for (struct addrinfo *b = found; b != NULL; b = b->ai_next) {
        if (b->ai_family == AF_INET) {
            a = b;
            break;
        }
    }
    uint32_t result = FAILED;
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            result = add(n, fd, false);
        else if (errno == EINPROGRESS)
            result = add(n, fd, true);
        else
            close(fd);
    }
    freeaddrinfo(found);
    return result;
}

// Listening only happens on the host's loopback address.
static uint32_t
listen_on(Net *n)
{
    if (!valid_port(n->port))
        return FAILED;
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
    return add(n, fd, false);
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
    uint32_t id = n->id;
    int fd = n->fds[id];
    n->ready[id] = false;
    if (c != NET_CLOSE && n->broken[id])
        return FAILED;
    switch (c) {
    case NET_ACCEPT: {
        int client = accept(fd, NULL, NULL);
        if (client < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? NOT_YET : FAILED;
        return add(n, client, false);
    }
    case NET_SEND: {
        uint8_t *p = buffer(n, m);
        if (p == NULL)
            return FAILED;
        if (n->connecting[id])
            return 0;
        ssize_t sent = send(fd, p, n->length, 0);
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            n->want_write[id] = true;
            return 0;
        }
        return sent < 0 ? FAILED : (uint32_t)sent;
    }
    case NET_RECEIVE: {
        uint8_t *p = buffer(n, m);
        if (p == NULL)
            return FAILED;
        if (n->connecting[id])
            return NOT_YET;
        ssize_t got = recv(fd, p, n->length, 0);
        if (got < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK ? NOT_YET : FAILED;
        return got == 0 ? FAILED : (uint32_t)got;
    }
    case NET_CLOSE:
        close(fd);
        n->fds[id] = -1;
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
        if (n->fds[i] >= 0 && !n->ready[i] && !n->broken[i]) {
            short events = n->want_write[i] ? POLLIN | POLLOUT : POLLIN;
            fds[count] = (struct pollfd){ .fd = n->fds[i], .events = events };
            ids[count++] = i;
        }
    }
    if (count == 0 || poll(fds, (nfds_t)count, 0) <= 0)
        return false;
    bool any = false;
    for (int i = 0; i < count; i++) {
        if (fds[i].revents == 0)
            continue;
        int id = ids[i];
        if (n->connecting[id]) {
            int error = 0;
            socklen_t size = sizeof(error);
            getsockopt(n->fds[id], SOL_SOCKET, SO_ERROR, &error, &size);
            n->connecting[id] = false;
            n->broken[id] = error != 0;
        }
        if (fds[i].revents & (POLLOUT | POLLERR | POLLHUP))
            n->want_write[id] = false;
        n->ready[id] = true;
        any = true;
    }
    return any;
}
