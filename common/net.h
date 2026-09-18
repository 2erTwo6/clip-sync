#ifndef CLIPSYNC_NET_H
#define CLIPSYNC_NET_H

#include <stddef.h>
#include <stdint.h>

#define CLIPSYNC_MAGIC 0x434C5053u /* "CLPS" */
#define CLIPSYNC_MAX_PAYLOAD (8u * 1024u * 1024u)

enum clipsync_msg_type {
    CLIPSYNC_MSG_HELLO = 1,
    CLIPSYNC_MSG_TEXT  = 2,
    CLIPSYNC_MSG_PING  = 3,
    CLIPSYNC_MSG_PONG  = 4,
    CLIPSYNC_MSG_BYE   = 5,
};

int net_connect_tcp(const char *host, const char *port, int timeout_ms);
int net_listen_tcp(const char *host, const char *port);
int net_accept(int listen_fd);
int net_set_timeout(int fd, int seconds);
int net_send_all(int fd, const void *data, size_t len);
int net_recv_all(int fd, void *data, size_t len);
int net_send_msg(int fd, uint32_t type, const void *payload, uint32_t len);
int net_recv_msg(int fd, uint32_t *type, uint8_t **payload, uint32_t *len);

#endif
