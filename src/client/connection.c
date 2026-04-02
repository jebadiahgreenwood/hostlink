#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include "connection.h"

static int wait_connect(int fd, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return -1;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}

int connect_unix(const char *path, int connect_timeout_ms) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    /* sun_path is 108 bytes on Linux */
    size_t plen = strlen(path);
    if (plen >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, plen + 1);

    if (connect_timeout_ms > 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int r = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
        if (r < 0) {
            if (wait_connect(fd, connect_timeout_ms) != 0) { close(fd); return -1; }
        }
        fcntl(fd, F_SETFL, flags);
    } else {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            close(fd); return -1;
        }
    }
    return fd;
}

int connect_tcp(const char *host, int port, int connect_timeout_ms) {
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        if (connect_timeout_ms > 0) {
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            int r = connect(fd, rp->ai_addr, rp->ai_addrlen);
            if (r < 0 && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
            if (r < 0) {
                if (wait_connect(fd, connect_timeout_ms) != 0) {
                    close(fd); fd = -1; continue;
                }
            }
            fcntl(fd, F_SETFL, flags);
        } else {
            if (connect(fd, rp->ai_addr, rp->ai_addrlen) != 0) {
                close(fd); fd = -1; continue;
            }
        }
        break;
    }
    freeaddrinfo(res);
    return fd;
}
