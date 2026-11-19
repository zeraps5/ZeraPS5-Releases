/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PS5C_NET_LOOPBACK_H
#define PS5C_NET_LOOPBACK_H
#include "hle/hle.h"
typedef enum net_local_op {
    NL_SOCKET,
    NL_BIND,
    NL_LISTEN,
    NL_CONNECT,
    NL_ACCEPT,
    NL_SEND,
    NL_SENDTO,
    NL_RECV,
    NL_RECVFROM,
    NL_SENDMSG,
    NL_RECVMSG,
    NL_GETNAME,
    NL_GETPEER,
    NL_SETOPT,
    NL_GETOPT,
    NL_SHUTDOWN,
    NL_SELECT,
    NL_CLOSE
} net_local_op;
bool net_loopback_init(void);
void net_loopback_call(hle_call *c, net_local_op op);
/* For SceNet calls, handle only sockets created by the optional UDP backend. */
bool net_lan_sce_call(hle_call *c, net_local_op op);
/* Return true only for a live socket; results are values or negative BSD errno. */
bool net_loopback_close(int32_t fd, int64_t *result);
bool net_loopback_io(hle_call *c, int32_t fd, guest_addr buf, uint64_t size, bool write,
                     bool positioned, int64_t *result);
bool net_loopback_watch(uint64_t eq, int32_t fd, int16_t filter, uint64_t low, uint64_t udata,
                        bool remove, int64_t *result);
/* Reserve the file descriptor namespace; caller must not hold the file table lock. */
bool kernel_socket_claim_fd(int32_t fd);
void kernel_socket_release_fd(int32_t fd);
int kernel_file_poll_fd(int32_t fd, bool write); /* -1 invalid, 0 pending, 1 ready */
bool net_loopback_poll_fd(int32_t fd,uint16_t events,uint16_t *ready);
#endif
