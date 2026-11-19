/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Offline libSceNet / libSceSsl / libSceHttp. net_loopback.c handles guest
 * TCP wake-up channels and the optional IPv4 UDP path. */
#include <string.h>

#include <stdio.h>
#include <stdlib.h>

#include "base/log.h"
#include "hle/hle.h"
#include "hle/net_loopback.h"
#include "core/runtime.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

#define HTTP_ERROR_NETWORK ((int32_t)0x80431063) /* connection failed */

static void ok(hle_call *c) { hle_ret_s32(c, 0); }

/* ids start at 1: 0 and negative values mean failure to the callers */
static void put_zero(hle_call *c, uint32_t arg, uint32_t size);

static void set_errno(hle_call *c, int e) {
    if (c->thread && c->thread->errno_addr) gmem_write(c->mem, c->thread->errno_addr, &e, 4);
}

#define SOCKET_WRAPPER(name, op) static void name(hle_call *c) { net_loopback_call(c, op); }
SOCKET_WRAPPER(posix_socket, NL_SOCKET)
SOCKET_WRAPPER(posix_bind, NL_BIND)
SOCKET_WRAPPER(posix_listen, NL_LISTEN)
SOCKET_WRAPPER(posix_connect, NL_CONNECT)
SOCKET_WRAPPER(posix_accept, NL_ACCEPT)
SOCKET_WRAPPER(posix_shutdown, NL_SHUTDOWN)
SOCKET_WRAPPER(posix_getsockname, NL_GETNAME)
SOCKET_WRAPPER(posix_getpeername, NL_GETPEER)
SOCKET_WRAPPER(posix_getsockopt, NL_GETOPT)
SOCKET_WRAPPER(posix_setsockopt, NL_SETOPT)
SOCKET_WRAPPER(posix_send, NL_SEND)
SOCKET_WRAPPER(posix_sendto, NL_SENDTO)
SOCKET_WRAPPER(posix_sendmsg, NL_SENDMSG)
SOCKET_WRAPPER(posix_recv, NL_RECV)
SOCKET_WRAPPER(posix_recvfrom, NL_RECVFROM)
SOCKET_WRAPPER(posix_recvmsg, NL_RECVMSG)
SOCKET_WRAPPER(posix_select, NL_SELECT)
#undef SOCKET_WRAPPER

#define NET_ERROR_NETDOWN ((int32_t)0x80410132)
static void net_down_net(hle_call *c) { hle_ret_s32(c, NET_ERROR_NETDOWN); }

/* Initialization makes the existing local transport available. It does not
 * imply an external network connection (NetCtl reports that separately). */
static void net_init(hle_call *c) { hle_ret_s32(c, net_loopback_init() ? 0 : NET_ERROR_NETDOWN); }

/* int sceNetCtlGetState(int *state): 0 = disconnected */
static void netctl_get_state(hle_call *c) {
    put_zero(c, 0, 4);
    hle_ret_s32(c, 0);
}

/* int sceNetCtlRegisterCallback(func, void *arg, int *cid) */
static void netctl_register_callback(hle_call *c) {
    int32_t *cid = hle_arg_ptr(c, 2) ? (int32_t *)hle_mem(c, hle_arg_ptr(c, 2), 4) : NULL;
    if (cid) *cid = 1;
    hle_ret_s32(c, 0);
}

/* int sceNetGetMemoryPoolStats(int memid, stats *), sceSslGetMemoryPoolStats(int ctx, stats *),
 * sceHttp2GetMemoryPoolStats(int ctx, stats *): stats { size_t pool, max_inuse, inuse; ... }, nothing used */
static void memory_pool_stats(hle_call *c) {
    put_zero(c, 1, 24);
    hle_ret_s32(c, 0);
}


#define NETCTL_ERROR_NOT_CONNECTED ((int32_t)0x80412108)
static void netctl_not_connected(hle_call *c) { hle_ret_s32(c, NETCTL_ERROR_NOT_CONNECTED); }

/* int sceNetGetMacAddress(SceNetEtherAddr *addr, int flags) */
static void net_get_mac_address(hle_call *c) {
    uint8_t *mac = hle_arg_ptr(c, 0) ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 6) : NULL;
    static const uint8_t k_mac[6] = {0x02, 0x00, 0x5A, 0x45, 0x52, 0x41}; /* locally administered */
    if (mac) memcpy(mac, k_mac, 6);
    hle_ret_s32(c, 0);
}

/* char *sceNetEtherNtostr(const SceNetEtherAddr *addr, char *str, size_t len) */
static void net_ether_ntostr(hle_call *c) {
    const uint8_t *mac = hle_arg_ptr(c, 0) ? (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), 6) : NULL;
    uint64_t len = hle_arg(c, 2);
    char *out = hle_arg_ptr(c, 1) && len ? (char *)hle_mem(c, hle_arg_ptr(c, 1), len) : NULL;
    if (mac && out) snprintf(out, (size_t)len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    hle_ret_u64(c, out ? hle_arg_ptr(c, 1) : 0);
}

/* int sceRandomGetRandomNumber(void *buf, size_t size) */
static void random_get_random_number(hle_call *c) {
    uint64_t size = hle_arg(c, 1);
    uint8_t *buf = hle_arg_ptr(c, 0) && size <= 64 ? (uint8_t *)hle_mem(c, hle_arg_ptr(c, 0), size) : NULL;
    if (!buf) {
        hle_ret_s32(c, (int32_t)0x817C0016); /* invalid argument */
        return;
    }
    static uint64_t state;
    if (!state) state = pal_time_ns() | 1;
    for (uint64_t i = 0; i < size; i++) { /* xorshift64* */
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        buf[i] = (uint8_t)((state * 0x2545F4914F6CDD1Dull) >> 56);
    }
    hle_ret_s32(c, 0);
}

/* int *sceNetErrnoLoc(void): the thread's errno serves as its net errno */
static void net_errno_loc(hle_call *c) { hle_ret_u64(c, c->thread ? c->thread->errno_addr : 0); }

/* const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) */
static void posix_inet_ntop(hle_call *c) {
    int32_t af = hle_arg_s32(c, 0);
    uint64_t size = hle_arg(c, 3);
    const uint8_t *a = hle_arg_ptr(c, 1) ? (const uint8_t *)hle_mem(c, hle_arg_ptr(c, 1), af == 28 ? 16 : 4) : NULL;
    char *out = hle_arg_ptr(c, 2) && size ? (char *)hle_mem(c, hle_arg_ptr(c, 2), size) : NULL;
    if (!a || !out || (af != 2 && af != 28)) {
        set_errno(c, 47); /* EAFNOSUPPORT */
        hle_ret_u64(c, 0);
        return;
    }
    char buf[48];
    if (af == 2) {
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    } else {
        int n = 0;
        for (int k = 0; k < 8; k++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, k ? ":%x" : "%x", (a[2 * k] << 8) | a[2 * k + 1]);
    }
    if (strlen(buf) + 1 > size) {
        set_errno(c, 28); /* ENOSPC */
        hle_ret_u64(c, 0);
        return;
    }
    memcpy(out, buf, strlen(buf) + 1);
    hle_ret_u64(c, hle_arg_ptr(c, 2));
}

/* Dotted quad: exactly four decimal parts 0-255 */
static bool parse_ipv4(const char *s, uint8_t out[4]) {
    for (int k = 0; k < 4; k++) {
        if (*s < '0' || *s > '9') return false;
        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) v = v * 10 + (unsigned)(*s++ - '0'), digits++;
        if (v > 255 || digits > 3) return false;
        out[k] = (uint8_t)v;
        if (k < 3 && *s++ != '.') return false;
    }
    return *s == 0;
}

/* Up to eight hex groups, one "::" for a run of zeros, an IPv4 tail allowed */
static bool parse_ipv6(const char *s, uint8_t out[16]) {
    uint8_t b[16] = {0};
    int n = 0, gap = -1;
    if (s[0] == ':' && s[1] == ':') gap = 0, s += 2;
    while (*s && n < 16) {
        const char *dot = strchr(s, '.');
        if (dot && !strchr(s, ':') && n <= 12) {
            if (!parse_ipv4(s, b + n)) return false;
            n += 4;
            s += strlen(s);
            break;
        }
        unsigned v = 0, digits = 0;
        for (; digits < 4; digits++, s++) {
            int h = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
            if (h < 0) break;
            v = v * 16 + (unsigned)h;
        }
        if (!digits) return false;
        b[n++] = (uint8_t)(v >> 8), b[n++] = (uint8_t)v;
        if (!*s) break;
        if (*s++ != ':') return false;
        if (*s == ':') {
            if (gap >= 0) return false;
            gap = n, s++;
            if (!*s) break;
        } else if (!*s) {
            return false;
        }
    }
    if (*s || (gap < 0 && n != 16) || (gap >= 0 && n > 14)) return false;
    if (gap >= 0) {
        memmove(b + 16 - (n - gap), b + gap, (size_t)(n - gap));
        memset(b + gap, 0, (size_t)(16 - n));
    }
    memcpy(out, b, 16);
    return true;
}

/* int inet_pton(int af, const char *src, void *dst): 1 converted, 0 not an
 * address, -1 unknown family (Minecraft checks its server addresses) */
static void posix_inet_pton(hle_call *c) {
    int32_t af = hle_arg_s32(c, 0);
    char src[64];
    uint8_t addr[16];
    if (af != 2 && af != 28) {
        set_errno(c, 47); /* EAFNOSUPPORT */
        hle_ret_s32(c, -1);
        return;
    }
    if (!hle_arg_ptr(c, 1) || !hle_read_str(c, hle_arg_ptr(c, 1), src, sizeof(src)) ||
        !(af == 2 ? parse_ipv4(src, addr) : parse_ipv6(src, addr))) {
        hle_ret_s32(c, 0);
        return;
    }
    if (hle_arg_ptr(c, 2)) gmem_write(c->mem, hle_arg_ptr(c, 2), addr, af == 2 ? 4 : 16);
    hle_ret_s32(c, 1);
}

/* int sceNetInetPtonEx(int af, const char *src, void *dst, int flags): sceNetInetPton; no
 * flag changes the parsing of the plain forms games pass */
static void inet_pton_ex(hle_call *c) { posix_inet_pton(c); }

/* int sceNetUsleep(SceNetUsecond usec) */
static void net_usleep(hle_call *c) {
    uint32_t us = hle_arg_u32(c, 0);
    pal_sleep_ns((uint64_t)us * 1000u);
    hle_ret_s32(c, 0);
}

/* int sceSslGetCaCerts(int ctx, {SceSslData *certs; size_t count; void *pool} *ca): no certificates */
static void ssl_get_ca_certs(hle_call *c) {
    void *ca = hle_arg_ptr(c, 1) ? hle_mem(c, hle_arg_ptr(c, 1), 24) : NULL;
    if (!ca) {
        hle_ret_s32(c, (int32_t)0x8095F006); /* invalid value */
        return;
    }
    memset(ca, 0, 24);
    hle_ret_s32(c, 0);
}

/* int sceHttpUriEscape(char *out, size_t *require, size_t prepare, const char *in): percent-encodes
 * every byte outside the unreserved set; with out NULL only the size (with the NUL) is reported */
static void http_uri_escape(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0), in = hle_arg_ptr(c, 3);
    uint64_t prepare = hle_arg(c, 2), need = 1;
    const char *hex = "0123456789ABCDEF";
    if (!in) {
        hle_ret_s32(c, (int32_t)0x80431100); /* invalid value */
        return;
    }
    for (uint64_t i = 0; i < 65536; i++) {
        const uint8_t *b = (const uint8_t *)hle_mem(c, in + i, 1);
        if (!b || !*b) break;
        bool keep = (*b >= 'A' && *b <= 'Z') || (*b >= 'a' && *b <= 'z') || (*b >= '0' && *b <= '9') ||
                    *b == '-' || *b == '_' || *b == '.' || *b == '~';
        char enc[3] = {(char)*b, 0, 0};
        uint32_t n = 1;
        if (!keep) enc[0] = '%', enc[1] = hex[*b >> 4], enc[2] = hex[*b & 15], n = 3;
        if (out && need - 1 + n < prepare) {
            char *d = (char *)hle_mem(c, out + need - 1, n);
            if (d) memcpy(d, enc, n);
        }
        need += n;
    }
    uint64_t *req = hle_arg_ptr(c, 1) ? (uint64_t *)hle_mem(c, hle_arg_ptr(c, 1), 8) : NULL;
    if (req) *req = need;
    if (out && need > prepare) {
        hle_ret_s32(c, (int32_t)0x80431101); /* out of size */
        return;
    }
    if (out) {
        char *z = (char *)hle_mem(c, out + need - 1, 1);
        if (z) *z = 0;
    }
    hle_ret_s32(c, 0);
}

static void new_id(hle_call *c) {
    static volatile int32_t next;
    hle_ret_s32(c, __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED));
}

static void net_down(hle_call *c) { hle_ret_s32(c, HTTP_ERROR_NETWORK); }

static void put_zero(hle_call *c, uint32_t arg, uint32_t size) {
    void *p = hle_arg_ptr(c, arg) ? hle_mem(c, hle_arg_ptr(c, arg), size) : NULL;
    if (p) memset(p, 0, size);
}

/* Non-blocking requests (sceHttpSetNonblock) with an epoll handle, as on the console: the first
 * sceHttpSendRequest answers EAGAIN, sceHttpWaitRequest then reports the failed connection, and
 * the next send returns the error. No Man's Sky's libHttpClient (PlayFab) expects that order: a
 * send failing at once sent it down a cleanup path that later locked a freed mutex and aborted. */
#define HTTP_ERROR_EAGAIN ((int32_t)0x80431082)
#define HTTP_NB_EVENT_SOCK_ERR 0x00000008u
#define MAX_NB_REQUESTS 64
typedef struct nb_request {
    int32_t id;
    bool nonblock;
    uint8_t state; /* 0 not sent, 1 in progress (EAGAIN given), 2 failure reported */
    uint64_t epoll, user_arg;
} nb_request;
static nb_request g_nb[MAX_NB_REQUESTS];
static volatile int32_t g_nb_lock;
static void nb_lock(void) { while (__atomic_test_and_set(&g_nb_lock, __ATOMIC_ACQUIRE)) pal_sleep_ns(0); }
static void nb_unlock(void) { __atomic_clear(&g_nb_lock, __ATOMIC_RELEASE); }
/* Caller holds the lock; `create` takes a free slot for an unknown id. */
static nb_request *nb_find(int32_t id, bool create) {
    nb_request *free_slot = NULL;
    for (uint32_t i = 0; i < MAX_NB_REQUESTS; i++) {
        if (g_nb[i].id == id) return &g_nb[i];
        if (!g_nb[i].id && !free_slot) free_slot = &g_nb[i];
    }
    if (!create || !free_slot || id <= 0) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->id = id;
    return free_slot;
}

/* int sceHttpSetNonblock(int id, bool enable) */
static void set_nonblock(hle_call *c) {
    nb_lock();
    nb_request *r = nb_find(hle_arg_s32(c, 0), true);
    if (r) r->nonblock = (hle_arg(c, 1) & 0xFF) != 0;
    nb_unlock();
    hle_ret_s32(c, 0);
}

/* int sceHttpCreateEpoll(int libhttpCtxId, SceHttpEpollHandle *eh) */
static void create_epoll(hle_call *c) {
    static volatile int32_t next;
    uint64_t eh = (uint64_t)__atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
    if (!hle_arg_ptr(c, 1) || !gmem_write(c->mem, hle_arg_ptr(c, 1), &eh, 8)) {
        hle_ret_s32(c, (int32_t)0x804311fe); /* invalid value */
        return;
    }
    hle_ret_s32(c, 0);
}

/* int sceHttpSetEpoll(int id, SceHttpEpollHandle eh, void *userArg) */
static void set_epoll(hle_call *c) {
    nb_lock();
    nb_request *r = nb_find(hle_arg_s32(c, 0), true);
    if (r) r->epoll = hle_arg(c, 1), r->user_arg = hle_arg(c, 2);
    nb_unlock();
    hle_ret_s32(c, 0);
}

/* int sceHttpUnsetEpoll(int id) */
static void unset_epoll(hle_call *c) {
    nb_lock();
    nb_request *r = nb_find(hle_arg_s32(c, 0), false);
    if (r) r->epoll = 0;
    nb_unlock();
    hle_ret_s32(c, 0);
}

/* int sceHttpDeleteRequest(int id) */
static void delete_request(hle_call *c) {
    nb_lock();
    nb_request *r = nb_find(hle_arg_s32(c, 0), false);
    if (r) memset(r, 0, sizeof(*r));
    nb_unlock();
    hle_ret_s32(c, 0);
}

/* int sceHttpWaitRequest(SceHttpEpollHandle eh, SceHttpNBEvent *nbev, int maxevents, int timeout_us):
 * SceHttpNBEvent { u32 events; u32 eventDetail; int id; void *userArg; } (24 bytes) */
static void wait_request(hle_call *c) {
    uint64_t eh = hle_arg(c, 0);
    guest_addr out = hle_arg_ptr(c, 1);
    int32_t max = hle_arg_s32(c, 2), timeout = hle_arg_s32(c, 3), n = 0;
    static int never = -1; /* PS5C_HTTP_PENDING: requests stay in flight (comparison) */
    if (never < 0) never = getenv("PS5C_HTTP_PENDING") != NULL;
    if (never) max = 0;
    nb_lock();
    for (uint32_t i = 0; out && i < MAX_NB_REQUESTS && n < max; i++) {
        nb_request *r = &g_nb[i];
        if (!r->id || r->state != 1 || r->epoll != eh) continue;
        uint8_t ev[24] = {0};
        uint32_t events = HTTP_NB_EVENT_SOCK_ERR;
        memcpy(ev, &events, 4);
        memcpy(ev + 8, &r->id, 4);
        memcpy(ev + 16, &r->user_arg, 8);
        if (!gmem_write(c->mem, out + (guest_addr)n * 24, ev, sizeof(ev))) break;
        r->state = 2; /* the next send returns the error */
        n++;
    }
    nb_unlock();
    if (!n && timeout != 0) /* nothing in flight: the wait times out */
        pal_sleep_ns(timeout > 0 && timeout < 16000 ? (uint64_t)timeout * 1000 : 16000000ull);
    hle_ret_s32(c, n);
}

static void send_request(hle_call *c) {
    static uint32_t logged;
    int32_t id = hle_arg_s32(c, 0);
    bool in_progress = false;
    nb_lock();
    nb_request *r = nb_find(id, false);
    if (r && r->epoll && r->state < 2) { /* epoll implies non-blocking (set on the template, inherited) */
        r->state = 1; /* the failure arrives through sceHttpWaitRequest */
        in_progress = true;
    }
    nb_unlock();
    if (in_progress) {
        hle_ret_s32(c, HTTP_ERROR_EAGAIN);
        return;
    }
    if (logged++ < 4) LOG_I(HLE, "sceHttpSendRequest: no network (request %d)", id);
    hle_ret_s32(c, HTTP_ERROR_NETWORK);
}

/* int sceHttpGetStatusCode(int req, int *status) */
static void get_status_code(hle_call *c) {
    put_zero(c, 1, 4);
    hle_ret_s32(c, HTTP_ERROR_NETWORK);
}

/* int sceHttpGetAllResponseHeaders(int req, char **header, size_t *size) */
static void get_all_response_headers(hle_call *c) {
    put_zero(c, 1, 8);
    put_zero(c, 2, 8);
    hle_ret_s32(c, HTTP_ERROR_NETWORK);
}

/* int sceHttpGetResponseContentLength(int req, int *result, uint64_t *length) */
static void get_content_length(hle_call *c) {
    put_zero(c, 1, 4);
    put_zero(c, 2, 8);
    hle_ret_s32(c, HTTP_ERROR_NETWORK);
}

#define HTTP_ERROR_OUT_OF_SIZE ((int32_t)0x80431104)
#define HTTP_ERROR_INVALID_VALUE ((int32_t)0x804311fe)
#define HTTP_ERROR_INVALID_URL ((int32_t)0x80433060)

/* int sceHttpUriParse(SceHttpUriElement *out, const char *uri, void *pool,
 * size_t *require, size_t prepare): splits a URI into its parts, copied as
 * strings into the pool. With no pool, only the size it needs is returned.
 * SceHttpUriElement: { bool opaque; char *scheme, *username, *password,
 * *hostname, *path, *query, *fragment; u16 port; u8 reserved[10] } (0x50)
 * (Super Bomberman R 2 parses its server address at start). */
static void uri_parse(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0), pool = hle_arg_ptr(c, 2), require = hle_arg_ptr(c, 3);
    uint64_t prepare = hle_arg(c, 4);
    char uri[2048];
    if (!hle_arg_ptr(c, 1) || !hle_read_str(c, hle_arg_ptr(c, 1), uri, sizeof(uri))) {
        hle_ret_s32(c, HTTP_ERROR_INVALID_VALUE);
        return;
    }
    /* scheme ":" ["//" [user [":" password] "@"] host [":" port]] path ["?" query] ["#" fragment] */
    const char *part[7] = {"", "", "", "", "", "", ""}; /* scheme user password host path query fragment */
    size_t len[7] = {0};
    char *s = uri, *colon = strchr(s, ':');
    bool opaque = false;
    uint32_t port = 0;
    const char *delim = strpbrk(s, "/?#");
    if (!colon || colon == s || (delim && delim < colon)) {
        hle_ret_s32(c, HTTP_ERROR_INVALID_URL);
        return;
    }
    part[0] = s, len[0] = (size_t)(colon - s);
    s = colon + 1;
    if (s[0] == '/' && s[1] == '/') {
        s += 2;
        char *auth_end = s + strcspn(s, "/?#"), *at = memchr(s, '@', (size_t)(auth_end - s));
        if (at) {
            char *pw = memchr(s, ':', (size_t)(at - s));
            part[1] = s, len[1] = (size_t)((pw ? pw : at) - s);
            if (pw) part[2] = pw + 1, len[2] = (size_t)(at - pw - 1);
            s = at + 1;
        }
        char *pc = memchr(s, ':', (size_t)(auth_end - s));
        part[3] = s, len[3] = (size_t)((pc ? pc : auth_end) - s);
        if (pc) port = (uint32_t)strtoul(pc + 1, NULL, 10);
        s = auth_end;
    } else {
        opaque = true;
    }
    part[4] = s, len[4] = strcspn(s, "?#");
    s += len[4];
    if (*s == '?') part[5] = s + 1, len[5] = strcspn(s + 1, "#"), s += 1 + len[5];
    if (*s == '#') part[6] = s + 1, len[6] = strlen(s + 1);
    if (!port) port = len[0] == 5 && !_strnicmp(part[0], "https", 5) ? 443 : len[0] == 4 && !_strnicmp(part[0], "http", 4) ? 80 : 0;
    size_t need = 0;
    for (int i = 0; i < 7; i++) need += len[i] + 1;
    if (require) gmem_write(c->mem, require, &(uint64_t){need}, 8);
    if (!pool) {
        hle_ret_s32(c, out ? HTTP_ERROR_INVALID_VALUE : 0);
        return;
    }
    if (prepare < need) {
        hle_ret_s32(c, HTTP_ERROR_OUT_OF_SIZE);
        return;
    }
    uint8_t elem[0x50];
    memset(elem, 0, sizeof(elem));
    elem[0] = opaque;
    guest_addr at = pool;
    for (int i = 0; i < 7; i++) {
        gmem_write(c->mem, at, part[i], len[i]);
        gmem_write(c->mem, at + len[i], "", 1);
        memcpy(elem + 8 + i * 8, &at, 8);
        at += len[i] + 1;
    }
    uint16_t port16 = (uint16_t)port;
    memcpy(elem + 0x40, &port16, 2);
    if (out) gmem_write(c->mem, out, elem, sizeof(elem));
    hle_ret_s32(c, 0);
}

/* int sceHttpUriBuild(char *out, size_t *require, size_t prepare, const SceHttpUriElement *in,
 * uint32_t option): the URI of the parts option names (1 scheme, 2 host, 4 port, 8 path,
 * 0x10 user, 0x20 password, 0x40 query, 0x80 fragment); a port only when it is not the
 * scheme's own. With out NULL only the size (with the NUL) is reported: Epic's online SDK
 * (Tetris Effect) asks for it first, and the size an unimplemented call left unset became
 * an 11 GiB allocation. */
static size_t uri_append(char *dst, size_t at, size_t cap, const char *src, size_t len) {
    for (size_t i = 0; i < len && at + i < cap; i++) dst[at + i] = src[i];
    return at + len;
}

static void uri_build(hle_call *c) {
    guest_addr out = hle_arg_ptr(c, 0), require = hle_arg_ptr(c, 1), in = hle_arg_ptr(c, 3);
    uint64_t prepare = hle_arg(c, 2);
    uint32_t option = hle_arg_u32(c, 4);
    uint8_t elem[0x50];
    if (!in || !gmem_read(c->mem, in, elem, sizeof(elem))) {
        hle_ret_s32(c, HTTP_ERROR_INVALID_URL);
        return;
    }
    if (!out && !require) {
        hle_ret_s32(c, HTTP_ERROR_INVALID_VALUE);
        return;
    }
    static const uint32_t wanted[7] = {0x01 | 0x04, 0x10, 0x20, 0x02, 0x08, 0x40, 0x80};
    char part[7][1024];
    size_t len[7];
    for (int i = 0; i < 7; i++) {
        guest_addr a;
        memcpy(&a, elem + 8 + i * 8, 8);
        part[i][0] = 0;
        if ((option & wanted[i]) && a && !hle_read_str(c, a, part[i], sizeof(part[i]))) part[i][0] = 0;
        len[i] = strlen(part[i]);
    }
    uint16_t port;
    memcpy(&port, elem + 0x40, 2);
    static char uri[8192];
    static volatile int32_t lock;
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {}
    size_t n = 0, cap = sizeof(uri) - 1;
    if ((option & 0x01) && len[0]) n = uri_append(uri, n, cap, part[0], len[0]), n = uri_append(uri, n, cap, ":", 1);
    if (elem[0] != 1) n = uri_append(uri, n, cap, "//", 2);
    n = uri_append(uri, n, cap, part[1], len[1]);
    if (len[2]) n = uri_append(uri, n, cap, ":", 1), n = uri_append(uri, n, cap, part[2], len[2]);
    if (len[1] || len[2]) n = uri_append(uri, n, cap, "@", 1);
    n = uri_append(uri, n, cap, part[3], len[3]);
    if ((option & 0x04) && port) {
        uint16_t own = len[0] == 5 && !_strnicmp(part[0], "https", 5) ? 443 : len[0] == 4 && !_strnicmp(part[0], "http", 4) ? 80 : 0;
        char num[8];
        if (port != own) n = uri_append(uri, n, cap, num, (size_t)snprintf(num, sizeof(num), ":%u", port));
    }
    for (int i = 4; i < 7; i++) n = uri_append(uri, n, cap, part[i], len[i]);
    if (n > cap) n = cap;
    uri[n] = 0;
    uint64_t need = n + 1;
    int32_t ret = 0;
    if (require) gmem_write(c->mem, require, &need, 8);
    if (out && prepare < need) ret = HTTP_ERROR_OUT_OF_SIZE;
    else if (out) gmem_write(c->mem, out, uri, need);
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
    hle_ret_s32(c, ret);
}

/* ---- byte order: pure functions, a stub returning 0 breaks every address ---- */
static void net_swap16(hle_call *c) { hle_ret_u64(c, __builtin_bswap16((uint16_t)hle_arg(c, 0))); }
static void net_swap32(hle_call *c) { hle_ret_u64(c, __builtin_bswap32((uint32_t)hle_arg(c, 0))); }
static void net_swap64(hle_call *c) { hle_ret_u64(c, __builtin_bswap64(hle_arg(c, 0))); }

/* Without the LAN backend, there is no SceNet socket descriptor. */
#define NET_ERROR_EBADF ((int32_t)0x80410109)
static void net_bad_socket(hle_call *c) {
    set_errno(c, 9); /* EBADF */
    hle_ret_s32(c, NET_ERROR_EBADF);
}

#define LAN_WRAPPER(name, op, offline) \
    static void name(hle_call *c) { if (!net_lan_sce_call(c, op)) offline(c); }
LAN_WRAPPER(net_close, NL_CLOSE, ok)
LAN_WRAPPER(net_sendto, NL_SENDTO, net_bad_socket)
LAN_WRAPPER(net_recvfrom, NL_RECVFROM, net_bad_socket)
#undef LAN_WRAPPER

/* int sceNetEpollWait(eid, events *, maxevents, int timeout_us): nothing ever becomes ready.
 * Waits like the console would (at most 100 ms per call) so polling loops do not spin. */
static void net_epoll_wait(hle_call *c) {
    int32_t timeout = hle_arg_s32(c, 3);
    uint64_t wait_us = timeout < 0 || timeout > 100000 ? 100000 : (uint64_t)timeout;
    if (wait_us) pal_sleep_ns(wait_us * 1000);
    hle_ret_s32(c, 0);
}

/* int sceNetResolverGetError(rid, int *status) */
static void net_resolver_get_error(hle_call *c) {
    int32_t *status = hle_arg_ptr(c, 1) ? (int32_t *)hle_mem(c, hle_arg_ptr(c, 1), 4) : NULL;
    if (status) *status = NET_ERROR_NETDOWN;
    hle_ret_s32(c, 0);
}

/* int sceNetCtlGetResult(int event, int *result), sceHttpGetLastErrno(int req, int *errno) */
static void zero_arg1_ok(hle_call *c) {
    put_zero(c, 1, 4);
    hle_ret_s32(c, 0);
}

/* sceSsl certificate queries: no connection ever gets a certificate */
#define SSL_ERROR_INVALID_CERT ((int32_t)0x8095F00B)
static void ssl_no_cert(hle_call *c) { hle_ret_s32(c, SSL_ERROR_INVALID_CERT); }
static void ssl_no_cert_ptr(hle_call *c) { hle_ret_u64(c, 0); }

/* NP session signaling needs the PSN peer network: no context is created */
static void signaling_down(hle_call *c) { hle_ret_s32(c, NET_ERROR_NETDOWN); }

#define H "libSceHttp"
#define H2 "libSceHttp2"

#define P "libScePosix"
#define POSIX_ENTRY(name,fn) HLE_FUNC_ENTRY(P,name,fn), HLE_FUNC_ENTRY("libkernel",name,fn)

static const hle_export k_exports[] = {
    HLE_FUNC_ENTRY("libSceNet", "sceNetInit", net_init),
    POSIX_ENTRY( "socket", posix_socket),
    POSIX_ENTRY( "select", posix_select),
    POSIX_ENTRY( "listen", posix_listen),
    POSIX_ENTRY( "shutdown", posix_shutdown),
    POSIX_ENTRY( "getsockopt", posix_getsockopt),
    POSIX_ENTRY( "send", posix_send),
    POSIX_ENTRY( "sendto", posix_sendto),
    POSIX_ENTRY( "sendmsg", posix_sendmsg),
    POSIX_ENTRY( "recv", posix_recv),
    POSIX_ENTRY( "recvfrom", posix_recvfrom),
    POSIX_ENTRY( "recvmsg", posix_recvmsg),
    POSIX_ENTRY( "accept", posix_accept),
    POSIX_ENTRY( "getpeername", posix_getpeername),
    POSIX_ENTRY( "inet_ntop", posix_inet_ntop),
    HLE_FUNC_ENTRY("libkernel", "__inet_ntop", posix_inet_ntop),
    HLE_FUNC_ENTRY("libkernel", "__inet_pton", posix_inet_pton),
    HLE_FUNC_ENTRY("libkernel", "inet_ntop", posix_inet_ntop),
    HLE_FUNC_ENTRY("libkernel", "inet_pton", posix_inet_pton),
    POSIX_ENTRY( "inet_pton", posix_inet_pton),
    HLE_FUNC_ENTRY("libSceNet", "sceNetInetPton", posix_inet_pton),
    HLE_FUNC_ENTRY("libSceNet", "sceNetInetPtonEx", inet_pton_ex),
    HLE_FUNC_ENTRY("libSceNet", "sceNetUsleep", net_usleep),
    HLE_FUNC_ENTRY("libSceNet", "sceNetErrnoLoc", net_errno_loc),
    HLE_FUNC_ENTRY("libSceNet", "sceNetInetNtop", posix_inet_ntop),
    POSIX_ENTRY( "connect", posix_connect),
    POSIX_ENTRY( "bind", posix_bind),
    POSIX_ENTRY( "setsockopt", posix_setsockopt),
    POSIX_ENTRY( "getsockname", posix_getsockname),
    HLE_FUNC_ENTRY("libSceNet", "sceNetSocket", net_down_net),
    HLE_FUNC_ENTRY("libSceNet", "sceNetSetsockopt", ok), /* no socket is ever created */
    HLE_FUNC_ENTRY("libSceNet", "sceNetSocketClose", net_close),
    HLE_FUNC_ENTRY("libSceNet", "sceNetResolverCreate", new_id),
    HLE_FUNC_ENTRY("libSceNet", "sceNetResolverStartNtoa", net_down_net),
    HLE_FUNC_ENTRY("libSceNet", "sceNetResolverStartAton", net_down_net),
    HLE_FUNC_ENTRY("libSceNet", "sceNetResolverDestroy", ok),
    HLE_FUNC_ENTRY("libSceNet", "sceNetGetMacAddress", net_get_mac_address),
    HLE_FUNC_ENTRY("libSceNet", "sceNetEtherNtostr", net_ether_ntostr),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlGetState", netctl_get_state),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlRegisterCallback", netctl_register_callback),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlUnregisterCallback", ok),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlCheckCallback", ok),
    HLE_FUNC_ENTRY("libSceNetCtlForNpToolkit", "sceNetCtlRegisterCallbackForNpToolkit", netctl_register_callback),
    HLE_FUNC_ENTRY("libSceNetCtlForNpToolkit", "sceNetCtlCheckCallbackForNpToolkit", ok),
    HLE_FUNC_ENTRY("libSceNetCtlForNpToolkit", "sceNetCtlUnregisterCallbackForNpToolkit", ok),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlGetInfo", netctl_not_connected),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlInit", ok),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlTerm", ok),
    /* IPv6: the same disconnected state */
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlGetStateV6", netctl_get_state),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlRegisterCallbackV6", netctl_register_callback),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlUnregisterCallbackV6", ok),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlCheckCallbackV6", ok),
    HLE_FUNC_ENTRY("libSceNet", "sceNetGetMemoryPoolStats", memory_pool_stats),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetMemoryPoolStats", memory_pool_stats),
    HLE_FUNC_ENTRY("libSceHttp2", "sceHttp2GetMemoryPoolStats", memory_pool_stats),
    HLE_FUNC_ENTRY("libSceNet", "sceNetHtons", net_swap16),
    HLE_FUNC_ENTRY("libSceNet", "sceNetNtohs", net_swap16),
    HLE_FUNC_ENTRY("libSceNet", "sceNetHtonl", net_swap32),
    HLE_FUNC_ENTRY("libSceNet", "sceNetNtohl", net_swap32),
    HLE_FUNC_ENTRY("libSceNet", "sceNetHtonll", net_swap64),
    HLE_FUNC_ENTRY("libSceNet", "sceNetNtohll", net_swap64),
    HLE_FUNC_ENTRY("libSceNet", "sceNetBind", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetConnect", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetListen", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetAccept", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetSend", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetSendto", net_sendto),
    HLE_FUNC_ENTRY("libSceNet", "sceNetRecv", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetRecvfrom", net_recvfrom),
    HLE_FUNC_ENTRY("libSceNet", "sceNetGetsockname", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetGetpeername", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetGetsockopt", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetGetSockInfo", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetGetSockInfo6", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetShutdown", net_bad_socket),
    HLE_FUNC_ENTRY("libSceNet", "sceNetSocketAbort", ok),
    HLE_FUNC_ENTRY("libSceNet", "sceNetEpollCreate", new_id),
    HLE_FUNC_ENTRY("libSceNet", "sceNetEpollControl", ok),
    HLE_FUNC_ENTRY("libSceNet", "sceNetEpollWait", net_epoll_wait),
    HLE_FUNC_ENTRY("libSceNet", "sceNetEpollAbort", ok),
    HLE_FUNC_ENTRY("libSceNet", "sceNetEpollDestroy", ok),
    HLE_FUNC_ENTRY("libSceNet", "sceNetResolverStartNtoaMultipleRecords", net_down_net),
    HLE_FUNC_ENTRY("libSceNet", "sceNetResolverGetError", net_resolver_get_error),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlGetNatInfo", netctl_not_connected),
    HLE_FUNC_ENTRY("libSceNetCtl", "sceNetCtlGetResult", zero_arg1_ok),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetSubjectName", ssl_no_cert_ptr),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetIssuerName", ssl_no_cert_ptr),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetNameEntryCount", ssl_no_cert),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetNameEntryInfo", ssl_no_cert),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetSerialNumber", ssl_no_cert),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetPem", ssl_no_cert),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslFreeSslCertName", ok),
    HLE_FUNC_ENTRY(H, "sceHttpGetLastErrno", zero_arg1_ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetChunkedTransferEnabled", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetAuthInfoCallback", ok),
    HLE_FUNC_ENTRY(H, "sceHttpsLoadCert", ok),
    HLE_FUNC_ENTRY(H, "sceHttpsUnloadCert", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2GetStatusCode", net_down),
    HLE_FUNC_ENTRY(H2, "sceHttp2GetAllResponseHeaders", net_down),
    HLE_FUNC_ENTRY(H2, "sceHttp2GetResponseContentLength", net_down),
    HLE_FUNC_ENTRY(H2, "sceHttp2WaitAsync", net_down),
    HLE_FUNC_ENTRY(H2, "sceHttp2CookieFlush", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetRequestNoContentLength", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetInflateGZIPEnabled", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetConnectionWaitTimeOut", ok),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingCreateContext", signaling_down),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingCreateContext2", signaling_down),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingDestroyContext", ok),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingActivateSession", signaling_down),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingActivateUser", signaling_down),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingDeactivate", ok),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingRequestPrepare", signaling_down),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingGetConnectionStatus", signaling_down),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingGetConnectionInfo", signaling_down),
    HLE_FUNC_ENTRY("libSceNpSessionSignaling", "sceNpSessionSignalingGetLocalNetInfo", signaling_down),
    HLE_FUNC_ENTRY("libSceRandom", "sceRandomGetRandomNumber", random_get_random_number),
    HLE_FUNC_ENTRY("libSceNet", "sceNetPoolCreate", new_id),
    HLE_FUNC_ENTRY("libSceNet", "sceNetPoolDestroy", ok),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslInit", new_id),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslTerm", ok),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslGetCaCerts", ssl_get_ca_certs),
    HLE_FUNC_ENTRY("libSceSsl", "sceSslFreeCaCerts", ok),
    HLE_FUNC_ENTRY(H, "sceHttpUriEscape", http_uri_escape),
    HLE_FUNC_ENTRY(H, "sceHttpInit", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpTerm", ok),
    HLE_FUNC_ENTRY(H, "sceHttpCreateTemplate", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpDeleteTemplate", ok),
    HLE_FUNC_ENTRY(H, "sceHttpCreateConnection", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpCreateConnectionWithURL", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpDeleteConnection", ok),
    HLE_FUNC_ENTRY(H, "sceHttpCreateRequest", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpCreateRequest2", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpCreateRequestWithURL", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpCreateRequestWithURL2", new_id),
    HLE_FUNC_ENTRY(H, "sceHttpDeleteRequest", delete_request),
    HLE_FUNC_ENTRY(H, "sceHttpAbortRequest", ok),
    HLE_FUNC_ENTRY(H, "sceHttpAddRequestHeader", ok),
    HLE_FUNC_ENTRY(H, "sceHttpRemoveRequestHeader", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetRequestContentLength", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetConnectTimeOut", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetSendTimeOut", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetRecvTimeOut", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetResolveTimeOut", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetResolveRetry", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetAutoRedirect", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetNonblock", set_nonblock),
    HLE_FUNC_ENTRY(H, "sceHttpCreateEpoll", create_epoll),
    HLE_FUNC_ENTRY(H, "sceHttpDestroyEpoll", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSetEpoll", set_epoll),
    HLE_FUNC_ENTRY(H, "sceHttpUnsetEpoll", unset_epoll),
    HLE_FUNC_ENTRY(H, "sceHttpWaitRequest", wait_request),
    HLE_FUNC_ENTRY(H, "sceHttpsSetSslCallback", ok),
    HLE_FUNC_ENTRY(H, "sceHttpsDisableOption", ok),
    HLE_FUNC_ENTRY(H, "sceHttpsEnableOption", ok),
    HLE_FUNC_ENTRY(H, "sceHttpSendRequest", send_request),
    HLE_FUNC_ENTRY(H, "sceHttpGetStatusCode", get_status_code),
    HLE_FUNC_ENTRY(H, "sceHttpGetAllResponseHeaders", get_all_response_headers),
    HLE_FUNC_ENTRY(H, "sceHttpGetResponseContentLength", get_content_length),
    HLE_FUNC_ENTRY(H, "sceHttpReadData", net_down),
    HLE_FUNC_ENTRY(H, "sceHttpUriParse", uri_parse),
    HLE_FUNC_ENTRY(H, "sceHttpUriBuild", uri_build),
    /* libSceHttp2: same model, ids from 1 (Unreal checks them), requests fail */
    HLE_FUNC_ENTRY(H2, "sceHttp2Init", new_id),
    HLE_FUNC_ENTRY(H2, "sceHttp2Term", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2CreateTemplate", new_id),
    HLE_FUNC_ENTRY(H2, "sceHttp2CreateCookieBox", new_id),
    HLE_FUNC_ENTRY(H2, "sceHttp2DeleteTemplate", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2CreateRequestWithURL", new_id),
    HLE_FUNC_ENTRY(H2, "sceHttp2DeleteRequest", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2AbortRequest", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2AddRequestHeader", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2RemoveRequestHeader", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetRequestContentLength", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetTimeOut", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetConnectTimeOut", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetSendTimeOut", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetRecvTimeOut", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetResolveTimeOut", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetResolveRetry", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetAutoRedirect", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetAuthEnabled", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetCookieBox", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetPreSendCallback", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetRedirectCallback", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetSslCallback", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SslEnableOption", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SslDisableOption", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SetMinSslVersion", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2SendRequest", net_down),
    /* async forms accept the request (Minecraft aborts on an immediate error); it never completes */
    HLE_FUNC_ENTRY(H2, "sceHttp2SendRequestAsync", ok),
    HLE_FUNC_ENTRY(H2, "sceHttp2ReadData", net_down),
    HLE_FUNC_ENTRY(H2, "sceHttp2ReadDataAsync", ok),
};

void hle_register_net_offline(void) {
    if (!net_loopback_init()) {
        LOG_E(HLE, "cannot initialize local socket channels");
        return;
    }
    hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports));
}
