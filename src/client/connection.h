#ifndef HOSTLINK_CONNECTION_H
#define HOSTLINK_CONNECTION_H

/* Connect to a Unix domain socket.
   connect_timeout_ms: how long to wait before giving up (-1 = blocking).
   Returns fd on success, -1 on error. */
int connect_unix(const char *path, int connect_timeout_ms);

/* Connect to a TCP host:port.
   Returns fd on success, -1 on error. */
int connect_tcp(const char *host, int port, int connect_timeout_ms);

#endif /* HOSTLINK_CONNECTION_H */
