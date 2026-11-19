/* SPDX-License-Identifier: GPL-2.0-or-later */
/* In-process TCP loopback for guest wake-up channels and opt-in Windows LAN UDP. */
#include "hle/net_loopback.h"
#include "core/runtime.h"
#include "hle/equeue.h"
#include "pal/pal.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "pal/udp_lan.h"
static bool lan_enabled;
#endif

#define FIRST_SOCKET 1000
#define SOCKETS 24
#define CAPACITY 65536
#define WATCHES 128
#define READ_FILTER (-1)
#define WRITE_FILTER (-2)
enum {
    N_BADF = 9,
    N_FAULT = 14,
    N_INVAL = 22,
    N_MFILE = 24,
    N_PIPE = 32,
    N_AGAIN = 35,
    N_AF = 47,
    N_INUSE = 48,
    N_ISCONN = 56,
    N_NOTCONN = 57,
    N_REFUSED = 61
};
typedef struct local_socket {
#ifdef _WIN32
    pal_udp_lan *udp;
#endif
    bool used, nonblock, listening, eof, read_closed, write_closed;
    int family, type, peer;
    uint64_t generation, peer_generation;
    uint16_t port;
    uint32_t address, head, count, backlog, pending_count;
    int pending[SOCKETS];
    uint8_t bytes[CAPACITY];
} local_socket;
static local_socket sockets[SOCKETS];
static struct {
    uint64_t eq, generation, low;
    int fd;
    int16_t filter;
} watches[WATCHES];
static pal_mutex *lock;
static pal_cond *changed;
static uint64_t generation;
static uint16_t next_port = 49152;
bool net_loopback_init(void) {
    if (!lock && pal_mutex_create(&lock) != PS5C_OK)
        return false;
    if (!changed && pal_cond_create(&changed) != PS5C_OK)
        return false;
#ifdef _WIN32
    lan_enabled = pal_udp_lan_init();
#endif
    return true;
}
static local_socket *get(int fd) {
    return fd >= FIRST_SOCKET && fd < FIRST_SOCKET + SOCKETS && sockets[fd - FIRST_SOCKET].used
               ? &sockets[fd - FIRST_SOCKET]
               : NULL;
}
static int allocate(int family, int type) {
    for (int i = 0; i < SOCKETS; i++)
        if (!sockets[i].used && kernel_socket_claim_fd(FIRST_SOCKET + i)) {
            local_socket *s = &sockets[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            s->family = family;
            s->type = type;
            s->peer = -1;
            s->generation = ++generation;
            return FIRST_SOCKET + i;
        }
    return -N_MFILE;
}
static local_socket *peer(local_socket *s) {
    local_socket *p = get(s->peer);
    return p && p->generation == s->peer_generation ? p : NULL;
}
static void notify(void) {
    for (unsigned i = 0; i < WATCHES; i++)
        if (watches[i].eq) {
            local_socket *s = get(watches[i].fd);
            if (!s || s->generation != watches[i].generation || !equeue_valid(watches[i].eq)) {
                watches[i].eq = 0;
                continue;
            }
            local_socket *p = peer(s);
            int64_t available = watches[i].filter == READ_FILTER
                                    ? (s->listening ? s->pending_count : s->count)
                                : p && !s->write_closed ? CAPACITY - p->count
                                                        : 0;
            if ((uint64_t)available >= (watches[i].low ? watches[i].low : 1) ||
                (watches[i].filter == READ_FILTER && s->eof))
                equeue_trigger(watches[i].eq, watches[i].fd, watches[i].filter, NULL, &available, 0,
                               false);
        }
    pal_cond_broadcast(changed);
}
static void close_locked(int fd) {
    local_socket *s = get(fd);
    if (!s)
        return;
    local_socket *p = peer(s);
    if (p) {
        p->peer = -1;
        p->eof = true;
    }
    while (s->pending_count)
        close_locked(s->pending[--s->pending_count]);
    for (unsigned i = 0; i < WATCHES; i++)
        if (watches[i].eq && watches[i].fd == fd) {
            equeue_remove(watches[i].eq, fd, watches[i].filter);
            watches[i].eq = 0;
        }
#ifdef _WIN32
    if (s->udp)
        pal_udp_lan_close(s->udp);
#endif
    s->used = false;
    kernel_socket_release_fd(fd);
}
bool net_loopback_close(int32_t fd, int64_t *result) {
    if (!lock || fd < FIRST_SOCKET || fd >= FIRST_SOCKET + SOCKETS)
        return false;
    pal_mutex_lock(lock);
    bool owned = get(fd) != NULL;
    if (owned) {
        close_locked(fd);
        notify();
        *result = 0;
    }
    pal_mutex_unlock(lock);
    return owned;
}
static bool address_arg(hle_call *c, unsigned arg, uint32_t len, uint32_t *address,
                        uint16_t *port) {
    const uint8_t *sa = len >= 16 ? hle_mem(c, hle_arg_ptr(c, arg), 16) : NULL;
    if (!sa || sa[1] != 2)
        return false;
    *port = (uint16_t)((sa[2] << 8) | sa[3]);
    *address = ((uint32_t)sa[4] << 24) | ((uint32_t)sa[5] << 16) | ((uint32_t)sa[6] << 8) | sa[7];
    return true;
}
static int output_address(hle_call *c, unsigned arg, uint32_t address, uint16_t port) {
    if (!hle_arg_ptr(c, arg))
        return 0;
    uint32_t *len = hle_mem(c, hle_arg_ptr(c, arg + 1), 4);
    if (!len)
        return -N_FAULT;
    uint32_t n = *len < 16 ? *len : 16;
    uint8_t *out = n ? hle_mem(c, hle_arg_ptr(c, arg), n) : NULL;
    if (n && !out)
        return -N_FAULT;
    uint8_t sa[16] = {16,
                      2,
                      (uint8_t)(port >> 8),
                      (uint8_t)port,
                      (uint8_t)(address >> 24),
                      (uint8_t)(address >> 16),
                      (uint8_t)(address >> 8),
                      (uint8_t)address};
    if (n)
        memcpy(out, sa, n);
    *len = 16;
    return 0;
}
static void auto_port(local_socket *s) {
    if (s->port)
        return;
    for (unsigned attempt = 0; attempt < SOCKETS + 1; attempt++) {
        uint16_t port = next_port++;
        if (next_port < 49152)
            next_port = 49152;
        bool used = false;
        for (unsigned k = 0; k < SOCKETS; k++)
            used |= sockets[k].used && sockets[k].port == port;
        if (!used) {
            s->port = port;
            return;
        }
    }
}
/* Caller holds lock. The ring is a stream: partial I/O, PEEK and EOF are real. */
static int64_t transfer_bytes(int fd, uint8_t *data, uint64_t size, unsigned flags, bool write) {
    local_socket *s = get(fd);
    if (!s)
        return -N_BADF;
    uint64_t id = s->generation;
    if (!size)
        return 0;
    if (s->type == 2)
        return write ? (int64_t)size : -N_AGAIN; /* existing offline UDP */
    for (;;) {
        s = get(fd);
        if (!s || s->generation != id)
            return -N_BADF;
        local_socket *p = peer(s);
        if (write) {
            if (s->write_closed)
                return -N_PIPE;
            if (!p)
                return s->eof ? -N_PIPE : -N_NOTCONN;
            if (p->read_closed)
                return -N_PIPE;
            uint32_t free = CAPACITY - p->count;
            if (free) {
                uint32_t n = (uint32_t)(size < free ? size : free),
                         at = (p->head + p->count) % CAPACITY;
                uint32_t first = n < CAPACITY - at ? n : CAPACITY - at;
                memcpy(p->bytes + at, data, first);
                memcpy(p->bytes, data + first, n - first);
                p->count += n;
                notify();
                return n;
            }
        } else {
            if (s->read_closed)
                return 0;
            if (s->count) {
                uint32_t n = (uint32_t)(size < s->count ? size : s->count),
                         first = n < CAPACITY - s->head ? n : CAPACITY - s->head;
                memcpy(data, s->bytes + s->head, first);
                memcpy(data + first, s->bytes, n - first);
                if (!(flags & 2)) {
                    s->head = (s->head + n) % CAPACITY;
                    s->count -= n;
                    notify();
                }
                return n;
            }
            if (s->eof)
                return 0;
            if (!p)
                return -N_NOTCONN;
        }
        if (s->nonblock || (flags & 0x80))
            return -N_AGAIN;
        pal_cond_wait(changed, lock);
    }
}
static int64_t transfer(hle_call *c, int fd, guest_addr buf, uint64_t size, unsigned flags,
                        bool write) {
    uint32_t capacity = (uint32_t)PS5C_MIN(size, (uint64_t)CAPACITY);
    if (capacity && !hle_mem(c, buf, capacity))
        return -N_FAULT;
    uint8_t *data = capacity ? malloc(capacity) : NULL;
    if (capacity && !data)
        return -12;
    if (write && capacity && !gmem_read(c->mem, buf, data, capacity)) {
        free(data);
        return -N_FAULT;
    }
    int64_t result = transfer_bytes(fd, data, capacity, flags, write);
    /* A blocking wait must never retain a host pointer into guest mappings. */
    if (!write && result > 0 && !gmem_write(c->mem, buf, data, (uint64_t)result))
        result = -N_FAULT;
    free(data);
    return result;
}
static int64_t vector_transfer(hle_call *c, int fd, bool write) {
    /* FreeBSD LP64 msghdr and iovec, independent of the host's ABI. */
    struct message {
        uint64_t name;
        uint32_t namelen, pad0;
        uint64_t iov;
        int32_t count;
        uint32_t pad1;
        uint64_t control;
        uint32_t controllen;
        int32_t flags;
    } msg;
    struct vector {
        uint64_t base, len;
    } vec[64];
    _Static_assert(sizeof(msg) == 48, "guest msghdr");
    if (!gmem_read(c->mem, hle_arg_ptr(c, 1), &msg, sizeof(msg)))
        return -N_FAULT;
    if (msg.count < 0 || msg.count > 64)
        return -N_INVAL;
    if (write && msg.controllen)
        return -45; /* unsupported ancillary data */
    if (msg.count && !gmem_read(c->mem, msg.iov, vec, (uint64_t)msg.count * sizeof(vec[0])))
        return -N_FAULT;
    uint64_t total = 0;
    for (int i = 0; i < msg.count; i++) {
        if (vec[i].len > UINT64_MAX - total)
            return -N_INVAL;
        total += vec[i].len;
    }
    uint32_t capacity = (uint32_t)PS5C_MIN(total, (uint64_t)CAPACITY);
    uint8_t *bytes = capacity ? malloc(capacity) : NULL;
    if (capacity && !bytes)
        return -12;
    uint32_t at = 0;
    for (int i = 0; i < msg.count && at < capacity; i++) {
        uint32_t n = (uint32_t)PS5C_MIN(vec[i].len, (uint64_t)(capacity - at));
        if (n && !hle_mem(c, vec[i].base, n)) {
            free(bytes);
            return -N_FAULT;
        }
        if (write && n && !gmem_read(c->mem, vec[i].base, bytes + at, n)) {
            free(bytes);
            return -N_FAULT;
        }
        at += n;
    }
    int64_t result = transfer_bytes(fd, bytes, capacity, hle_arg_u32(c, 2), write);
    if (!write && result >= 0) {
        at = 0;
        for (int i = 0; i < msg.count && at < (uint64_t)result; i++) {
            uint32_t n = (uint32_t)PS5C_MIN(vec[i].len, (uint64_t)result - at);
            if (n && !gmem_write(c->mem, vec[i].base, bytes + at, n)) {
                result = -N_FAULT;
                break;
            }
            at += n;
        }
        msg.namelen = 0;
        msg.controllen = 0;
        msg.flags = 0;
        if (result >= 0 && !gmem_write(c->mem, hle_arg_ptr(c, 1), &msg, sizeof(msg)))
            result = -N_FAULT;
    }
    free(bytes);
    return result;
}
bool net_loopback_io(hle_call *c, int32_t fd, guest_addr buf, uint64_t size, bool write,
                     bool positioned, int64_t *result) {
    if (!lock || fd < FIRST_SOCKET || fd >= FIRST_SOCKET + SOCKETS)
        return false;
    pal_mutex_lock(lock);
    bool owned = get(fd) != NULL;
    if (owned)
        *result = positioned ? -29 : transfer(c, fd, buf, size, 0, write);
    pal_mutex_unlock(lock);
    return owned;
}
bool net_loopback_watch(uint64_t eq, int32_t fd, int16_t filter, uint64_t low, uint64_t udata,
                        bool remove, int64_t *result) {
    if (!lock || fd < FIRST_SOCKET || fd >= FIRST_SOCKET + SOCKETS)
        return false;
    pal_mutex_lock(lock);
    local_socket *s = get(fd);
    bool owned = s != NULL;
    if (owned) {
        *result = -N_BADF;
        if (equeue_valid(eq)) {
            unsigned slot = WATCHES;
            for (unsigned i = 0; i < WATCHES; i++) {
                if (watches[i].eq == eq && watches[i].fd == fd && watches[i].filter == filter) {
                    slot = i;
                    break;
                }
                if (!watches[i].eq || !equeue_valid(watches[i].eq))
                    slot = i;
            }
            if (remove) {
                equeue_remove(eq, fd, filter);
                for (unsigned i = 0; i < WATCHES; i++)
                    if (watches[i].eq == eq && watches[i].fd == fd && watches[i].filter == filter)
                        watches[i].eq = 0;
                *result = 0;
            } else if (slot < WATCHES && equeue_add(eq, fd, filter, udata)) {
                watches[slot].eq = eq;
                watches[slot].fd = fd;
                watches[slot].generation = s->generation;
                watches[slot].filter = filter;
                watches[slot].low = low;
                *result = 0;
                notify();
            } else
                *result = -12;
        }
    }
    pal_mutex_unlock(lock);
    return owned;
}
static void ret(hle_call *c, int64_t result) {
    if (result < 0) {
        int error = (int)-result;
        if (c->thread && c->thread->errno_addr)
            gmem_write(c->mem, c->thread->errno_addr, &error, 4);
        hle_ret_u64(c, UINT64_MAX);
    } else
        hle_ret_u64(c, (uint64_t)result);
}
bool net_loopback_poll_fd(int32_t fd,uint16_t events,uint16_t *ready) {
    if(!lock)return false;
    pal_mutex_lock(lock);local_socket *s=get(fd);uint16_t result=0;
    if(s){local_socket *p=peer(s);
        if((events&0x41) && (s->listening?s->pending_count>0:s->count>0||s->eof||s->read_closed))result|=events&0x41;
        if((events&4) && (s->type==2||s->write_closed||(!p&&s->eof)||(p&&(p->read_closed||p->count<CAPACITY))))result|=4;
        if(s->eof)result|=0x10;
    }
    *ready=result;bool found=s!=NULL;pal_mutex_unlock(lock);return found;
}
static int64_t select_local(hle_call *c) {
    int nfds = hle_arg_s32(c, 0);
    if (nfds < 0 || nfds > 1024)
        return -N_INVAL;
    uint32_t bytes = (uint32_t)(nfds + 7) / 8;
    uint8_t requested[3][128] = {{0}}, ready[3][128];
    for (unsigned set = 0; set < 3; set++)
        if (bytes && hle_arg_ptr(c, set + 1) &&
            !gmem_read(c->mem, hle_arg_ptr(c, set + 1), requested[set], bytes))
            return -N_FAULT;
    uint64_t timeout = UINT64_MAX;
    if (hle_arg_ptr(c, 4)) {
        int64_t tv[2];
        if (!gmem_read(c->mem, hle_arg_ptr(c, 4), tv, sizeof(tv)))
            return -N_FAULT;
        if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000)
            return -N_INVAL;
        timeout = (uint64_t)tv[0] > (UINT64_MAX - (uint64_t)tv[1] * 1000) / 1000000000
                      ? UINT64_MAX
                      : (uint64_t)tv[0] * 1000000000 + (uint64_t)tv[1] * 1000;
    }
    uint64_t start = pal_time_ns();
    for (;;) {
        memset(ready, 0, sizeof(ready));
        int count = 0;
        for (int fd = 0; fd < nfds; fd++)
            for (unsigned set = 0; set < 3; set++) {
                uint8_t mask = (uint8_t)(1u << (fd & 7));
                if (!(requested[set][fd / 8] & mask))
                    continue;
                local_socket *s = get(fd);
                bool available = false;
                if (s) {
                    local_socket *p = peer(s);
                    if (set == 0)
                        available = s->listening ? s->pending_count > 0
                                                 : s->count > 0 || s->eof || s->read_closed;
                    if (set == 1)
                        available = s->type == 2 || s->write_closed || (!p && s->eof) ||
                                    (p && (p->read_closed || p->count < CAPACITY));
                } else {
                    int file = kernel_file_poll_fd(fd, set == 1);
                    if (file < 0)
                        return -N_BADF;
                    available = set < 2 && file > 0;
                }
                if (available) {
                    ready[set][fd / 8] |= mask;
                    count++;
                }
            }
        uint64_t elapsed = pal_time_ns() - start;
        if (count || elapsed >= timeout) {
            for (unsigned set = 0; set < 3; set++)
                if (bytes && hle_arg_ptr(c, set + 1) &&
                    !gmem_write(c->mem, hle_arg_ptr(c, set + 1), ready[set], bytes))
                    return -N_FAULT;
            return count;
        }
        if (timeout == UINT64_MAX)
            pal_cond_wait(changed, lock);
        else
            pal_cond_wait_ns(changed, lock, timeout - elapsed);
    }
}
#ifdef _WIN32
static int allocate_lan(void) {
    int fd = allocate(2, 2);
    if (fd < 0)
        return fd;
    int error = pal_udp_lan_open(&get(fd)->udp);
    if (error) {
        close_locked(fd);
        return error;
    }
    return fd;
}

static int64_t lan_datagram(hle_call *c, pal_udp_lan *udp, bool send) {
    /* This path only supports nonblocking datagram I/O. */
    if (hle_arg_u32(c, 3) != 0x80) /* MSG_DONTWAIT */
        return -45;
    uint64_t size = hle_arg(c, 2);
    guest_addr buffer = hle_arg_ptr(c, 1);
    uint32_t address = 0;
    uint16_t port = 0;
    if (send && !address_arg(c, 4, hle_arg_u32(c, 5), &address, &port))
        return -N_INVAL;
    if (send && size > 65507)
        return -40; /* EMSGSIZE */
    size = PS5C_MIN(size, 65507u);
    if (size && !hle_mem(c, buffer, size))
        return -N_FAULT;
    uint8_t *data = malloc(size ? size : 1);
    if (!data)
        return -12;
    int result;
    if (send) {
        result = size && !gmem_read(c->mem, buffer, data, size)
                     ? -N_FAULT : pal_udp_lan_send(udp, data, (int)size, address, port);
    } else {
        result = pal_udp_lan_receive(udp, data, (int)size, &address, &port);
        if (result >= 0) {
            int error = output_address(c, 4, address, port);
            if (error || (result && !gmem_write(c->mem, buffer, data, (uint64_t)result)))
                result = -N_FAULT;
        }
    }
    free(data);
    return result;
}

static int64_t lan_call(hle_call *c, net_local_op op, local_socket *s) {
    uint32_t address;
    uint16_t port;
    switch (op) {
    case NL_BIND:
        if (!address_arg(c, 1, hle_arg_u32(c, 2), &address, &port))
            return -N_INVAL;
        return pal_udp_lan_bind(s->udp, address, port);
    case NL_GETNAME: {
        if (!hle_arg_ptr(c, 1) || !hle_arg_ptr(c, 2))
            return -N_FAULT;
        int error = pal_udp_lan_name(s->udp, &address, &port);
        return error ? error : output_address(c, 1, address, port);
    }
    case NL_SENDTO:
    case NL_RECVFROM:
        return lan_datagram(c, s->udp, op == NL_SENDTO);
    case NL_SETOPT: {
        if (hle_arg_u32(c, 1) != 0 || hle_arg_u32(c, 2) != 12)
            return -45; /* only IPPROTO_IP / IP_ADD_MEMBERSHIP */
        if (hle_arg_u32(c, 4) != 8)
            return -N_INVAL;
        uint32_t request[2];
        if (!gmem_read(c->mem, hle_arg_ptr(c, 3), request, sizeof(request)))
            return -N_FAULT;
        return pal_udp_lan_join_group(__builtin_bswap32(request[0]),
                                      __builtin_bswap32(request[1]));
    }
    case NL_CLOSE:
        close_locked(hle_arg_s32(c, 0));
        notify();
        return 0;
    default:
        return -45; /* EOPNOTSUPP */
    }
}

bool net_lan_sce_call(hle_call *c, net_local_op op) {
    if (!lan_enabled)
        return false;
    pal_mutex_lock(lock);
    local_socket *s = get(hle_arg_s32(c, 0));
    bool handled = s && s->udp;
    if (handled) {
        int64_t result = lan_call(c, op, s);
        if (result < 0) {
            int error = (int)-result;
            if (c->thread && c->thread->errno_addr)
                gmem_write(c->mem, c->thread->errno_addr, &error, 4);
            hle_ret_s32(c, (int32_t)(0x80410100u | (uint32_t)error));
        } else {
            hle_ret_u64(c, (uint64_t)result);
        }
    }
    pal_mutex_unlock(lock);
    return handled;
}
#else
bool net_lan_sce_call(hle_call *c, net_local_op op) {
    (void)c;
    (void)op;
    return false;
}
#endif

void net_loopback_call(hle_call *c, net_local_op op) {
    if (!lock || !changed) {
        ret(c, -12);
        return;
    }
    pal_mutex_lock(lock);
    int fd = hle_arg_s32(c, 0);
    local_socket *s = get(fd);
    int64_t result = -N_BADF;
    uint32_t address = 0;
    uint16_t port = 0;
    if (op == NL_SELECT)
        result = select_local(c);
    else if (op == NL_SOCKET) {
        int type = hle_arg_s32(c, 1), protocol = hle_arg_s32(c, 2);
#ifdef _WIN32
        if (lan_enabled && fd == 2 && type == 2 && (!protocol || protocol == 17))
            result = allocate_lan();
        else
#endif
            result = (fd != 2 && fd != 28)                            ? -N_AF
                     : (type != 1 && type != 2)                       ? -N_INVAL
                     : (protocol && protocol != (type == 1 ? 6 : 17)) ? -N_INVAL
                                                                      : allocate(fd, type);
    }
#ifdef _WIN32
    else if (s && s->udp)
        result = lan_call(c, op, s);
#endif
    else if (s)
        switch (op) {
        case NL_BIND:
            if (!address_arg(c, 1, hle_arg_u32(c, 2), &address, &port)) {
                result = -N_INVAL;
                break;
            }
            if (s->port) {
                result = -N_INVAL;
                break;
            }
            result = 0;
            for (unsigned i = 0; i < SOCKETS; i++)
                if (sockets[i].used && sockets[i].port == port && port &&
                    (!address || !sockets[i].address || sockets[i].address == address)) {
                    result = -N_INUSE;
                    break;
                }
            if (!result) {
                s->address = address;
                s->port = port;
                auto_port(s);
            }
            break;
        case NL_LISTEN:
            if (s->type != 1 || peer(s)) {
                result = -N_INVAL;
                break;
            }
            auto_port(s);
            s->listening = true;
            s->backlog = (uint32_t)PS5C_MAX(1, PS5C_MIN(hle_arg_s32(c, 1), SOCKETS));
            result = 0;
            break;
        case NL_CONNECT: {
            result = -N_REFUSED;
            if (!address_arg(c, 1, hle_arg_u32(c, 2), &address, &port)) {
                result = -N_INVAL;
                break;
            }
            if (s->peer >= 0) {
                result = -N_ISCONN;
                break;
            }
            if (s->type != 1 || (address >> 24) != 127)
                break;
            local_socket *listener = NULL;
            for (unsigned i = 0; i < SOCKETS; i++)
                if (sockets[i].used && sockets[i].listening && sockets[i].port == port &&
                    (!sockets[i].address || sockets[i].address == address)) {
                    listener = &sockets[i];
                    break;
                }
            if (!listener || listener->pending_count >= listener->backlog)
                break;
            int accepted = allocate(2, 1);
            if (accepted < 0) {
                result = accepted;
                break;
            }
            local_socket *p = get(accepted);
            auto_port(s);
            if (!s->address)
                s->address = 0x7f000001;
            p->port = port;
            p->address = address;
            p->peer = fd;
            p->peer_generation = s->generation;
            p->nonblock = listener->nonblock;
            s->peer = accepted;
            s->peer_generation = p->generation;
            s->eof = false;
            listener->pending[listener->pending_count++] = accepted;
            result = 0;
            notify();
            break;
        }
        case NL_ACCEPT: {
            if (!s->listening) {
                result = -N_INVAL;
                break;
            }
            uint64_t id = s->generation;
            while (!s->pending_count && !s->nonblock) {
                pal_cond_wait(changed, lock);
                s = get(fd);
                if (!s || s->generation != id)
                    break;
            }
            if (!s || s->generation != id) {
                result = -N_BADF;
                break;
            }
            if (!s->pending_count) {
                result = -N_AGAIN;
                break;
            }
            int accepted = s->pending[0];
            local_socket *p = get(accepted), *client = p ? peer(p) : NULL;
            result = output_address(c, 1, client ? client->address : 0, client ? client->port : 0);
            if (!result) {
                memmove(s->pending, s->pending + 1, --s->pending_count * sizeof(s->pending[0]));
                result = accepted;
                notify();
            }
            break;
        }
        case NL_SEND:
        case NL_SENDTO:
        case NL_RECV:
        case NL_RECVFROM:
            result = transfer(c, fd, hle_arg_ptr(c, 1), hle_arg(c, 2), hle_arg_u32(c, 3),
                              op == NL_SEND || op == NL_SENDTO);
            if (op == NL_RECVFROM && result >= 0 && hle_arg_ptr(c, 4)) {
                local_socket *p = peer(s);
                int e = output_address(c, 4, p ? p->address : 0, p ? p->port : 0);
                if (e)
                    result = e;
            }
            break;
        case NL_GETNAME:
        case NL_GETPEER: {
            if (!hle_arg_ptr(c, 1) || !hle_arg_ptr(c, 2)) {
                result = -N_FAULT;
                break;
            }
            local_socket *p = op == NL_GETPEER ? peer(s) : s;
            result = p ? output_address(c, 1, p->address, p->port) : -N_NOTCONN;
            break;
        }
        case NL_SETOPT: {
            const int32_t *value = hle_arg_u32(c, 4) >= 4 ? hle_mem(c, hle_arg_ptr(c, 3), 4) : NULL;
            if (!value) {
                result = -N_FAULT;
                break;
            }
            if (hle_arg_u32(c, 1) == 0xffff && hle_arg_u32(c, 2) == 0x1200)
                s->nonblock = *value != 0;
            result = 0;
            break;
        }
        case NL_GETOPT: {
            uint32_t *len = hle_mem(c, hle_arg_ptr(c, 4), 4);
            uint32_t n = len ? PS5C_MIN(*len, 4u) : 0;
            uint8_t *out = n ? hle_mem(c, hle_arg_ptr(c, 3), n) : NULL;
            if (!len || (n && !out)) {
                result = -N_FAULT;
                break;
            }
            int value = hle_arg_u32(c, 2) == 0x1008   ? s->type
                        : hle_arg_u32(c, 2) == 0x1200 ? s->nonblock
                                                      : 0;
            if (n)
                memcpy(out, &value, n);
            *len = 4;
            result = 0;
            break;
        }
        case NL_SHUTDOWN: {
            int how = hle_arg_s32(c, 1);
            result = 0;
            if (how < 0 || how > 2) {
                result = -N_INVAL;
                break;
            }
            if (how != 1) {
                s->read_closed = true;
                s->count = 0;
            }
            if (how != 0) {
                s->write_closed = true;
                local_socket *p = peer(s);
                if (p)
                    p->eof = true;
            }
            notify();
            break;
        }
        case NL_SENDMSG:
        case NL_RECVMSG: result = vector_transfer(c, fd, op == NL_SENDMSG); break;
        default: break;
        }
    pal_mutex_unlock(lock);
    ret(c, result);
}
