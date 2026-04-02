#ifndef HOSTLINK_SERVER_H
#define HOSTLINK_SERVER_H

#include "../common/config.h"

/* Run the main server event loop. Does not return unless fatal error.
   cfg is a pointer to the live config (may be updated on SIGHUP).
   config_path is the file to re-read on SIGHUP. */
int server_run(daemon_config_t *cfg, int unix_fd, int tcp_fd,
               const char *config_path);

#endif /* HOSTLINK_SERVER_H */
