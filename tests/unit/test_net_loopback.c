/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "core/runtime.h"
#include "hle/equeue.h"
#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"
#include "test.h"
#include <string.h>

static ps5_runtime rt;
static guest_thread thread;
static guest_addr page;
static int64_t call(const char *lib, const char *name, uint64_t a, uint64_t b, uint64_t c,
                    uint64_t d, uint64_t e) {
    const hle_export *exp = hle_find(lib, nid_hash_name(name));
    if (!exp || !exp->fn)
        return INT64_MIN;
    hle_args args = {.gpr = {a, b, c, d, e}};
    hle_call ctx = {.rt = &rt, .mem = &rt.mem, .thread = &thread, .a = &args};
    exp->fn(&ctx);
    return (int64_t)args.ret[0];
}
#define N(name, a, b, c, d, e) call("libScePosix", name, a, b, c, d, e)
#define K(name, a, b, c, d, e) call("libkernel", "sceKernel" name, a, b, c, d, e)
static int error(void) {
    int e = 0;
    gmem_read(&rt.mem, thread.errno_addr, &e, 4);
    return e;
}
static void address(uint16_t port, uint8_t first) {
    uint8_t a[16] = {0, 2, (uint8_t)(port >> 8), (uint8_t)port, first, 0, 0, 1};
    gmem_write(&rt.mem, page, a, 16); /* sin_len zero, as used by a native caller */
}
static void nonblock(int fd) {
    int one = 1;
    gmem_write(&rt.mem, page + 64, &one, 4);
    CHECK(N("setsockopt", fd, 0xffff, 0x1200, page + 64, 4) == 0);
}
static int listener, client, accepted;
static void connect_local_channel(void) {
    CHECK(call("libSceNet","sceNetInit",0,0,0,0,0)==0);
    listener = (int)N("socket", 2, 1, 0, 0, 0);
    client = (int)N("socket", 2, 1, 0, 0, 0);
    CHECK(listener >= 0 && client >= 0 && listener != client);
    nonblock(listener);
    nonblock(client);
    address(32917, 127);
    CHECK(N("bind", listener, page, 16, 0, 0) == 0);
    CHECK(N("listen", listener, 4, 0, 0, 0) == 0);
    CHECK(N("accept", listener, 0, 0, 0, 0) == -1 && error() == 35);
    CHECK(N("connect", client, page, 16, 0, 0) == 0);
    accepted = (int)N("accept", listener, 0, 0, 0, 0);
    CHECK(accepted >= 0 && accepted != client && accepted != listener);
    nonblock(accepted);
    uint32_t len = 8;
    uint8_t out[20];
    memset(out, 0xa5, sizeof(out));
    gmem_write(&rt.mem, page + 80, out, sizeof(out));
    gmem_write(&rt.mem, page + 104, &len, 4);
    CHECK(N("getpeername", client, page + 80, page + 104, 0, 0) == 0);
    gmem_read(&rt.mem, page + 80, out, sizeof(out));
    gmem_read(&rt.mem, page + 104, &len, 4);
    CHECK(len == 16 && out[1] == 2 && out[2] == (32917 >> 8) && out[3] == (32917 & 255));
    CHECK(out[4] == 127 && out[7] == 1 && out[8] == 0xa5 && out[19] == 0xa5);
}
static void bytes_and_readiness(void) {
    uint64_t eq = equeue_create("local wake channel");
    CHECK(eq);
    CHECK(K("AddReadEvent", eq, accepted, 1, 0x12345678, 0) == 0);
    guest_kevent ev = {0};
    CHECK(equeue_wait(eq, &ev, 1, 0) == 0);
    const char text[] = "wake123";
    char out[8] = {0};
    gmem_write(&rt.mem, page + 128, text, 7);
    CHECK(N("send", client, page + 128, 7, 0, 0) == 7);
    CHECK(equeue_wait(eq, &ev, 1, 0) == 1 && ev.filter == -1 && ev.ident == (uint64_t)accepted);
    CHECK(ev.data == 7 && ev.udata == 0x12345678);
    CHECK(N("recv", accepted, page + 256, 3, 2, 0) == 3); /* PEEK */
    gmem_read(&rt.mem, page + 256, out, 3);
    CHECK(!memcmp(out, text, 3));
    CHECK(K("Pread", accepted, page + 256, 3, 0, 0) ==
          (int32_t)0x8002001d); /* ESPIPE, consumes nothing */
    CHECK(K("Read", accepted, page + 256, 3, 0, 0) == 3);
    CHECK(N("recv", accepted, page + 259, 4, 0, 0) == 4);
    gmem_read(&rt.mem, page + 256, out, 7);
    CHECK(!memcmp(out, text, 7));
    CHECK(N("recv", accepted, page + 256, 1, 0, 0) == -1 && error() == 35);
    /* Writable file-style APIs also reach the socket, and a zero-length I/O
     * must not inspect the supplied buffer pointer. */
    CHECK(K("Write", accepted, page + 128, 7, 0, 0) == 7);
    CHECK(N("recv", client, page + 256, 7, 0, 0) == 7);
    CHECK(N("send", client, 0, 0, 0, 0) == 0);
    CHECK(N("send", client, 0, 1, 0, 0) == -1 && error() == 14);
    CHECK(K("DeleteReadEvent", eq, accepted, 0, 0, 0) == 0);
    CHECK(equeue_destroy(eq));
}
static void stream_wrap_and_backpressure(void) {
    uint8_t *data = gmem_host(&rt.mem, page + 4096, 100000),
            *out = gmem_host(&rt.mem, page + 110000, 100000);
    CHECK(data && out);
    for (unsigned i = 0; i < 100000; i++)
        data[i] = (uint8_t)(i * 37);
    int64_t sent = N("send", client, page + 4096, 100000, 0, 0);
    CHECK(sent > 0 && sent < 100000);
    CHECK(N("send", client, page + 4096, 1, 0, 0) == -1 && error() == 35);
    CHECK(N("recv", accepted, page + 110000, 50000, 0, 0) == 50000);
    CHECK(!memcmp(data, out, 50000));
    CHECK(N("send", client, page + 4096 + sent, 100000 - sent, 0, 0) == 100000 - sent);
    int64_t received = 50000;
    while (received < 100000) {
        int64_t n = N("recv", accepted, page + 110000 + received, 100000 - received, 0, 0);
        CHECK(n > 0);
        received += n;
    }
    CHECK(!memcmp(data, out, 100000));
}
static void vectored_peek_and_consumption(void) {
    struct message {
        uint64_t name;
        uint32_t namelen, pad0;
        uint64_t iov;
        int32_t count;
        uint32_t pad1;
        uint64_t control;
        uint32_t controllen;
        int32_t flags;
    } msg = {.iov = page + 640, .count = 2};
    struct vector {
        uint64_t addr, len;
    } vec[2] = {{page + 128, 3}, {page + 131, 4}};
    gmem_write(&rt.mem, page + 128, "wake123", 7);
    gmem_write(&rt.mem, page + 512, &msg, sizeof(msg));
    gmem_write(&rt.mem, page + 640, vec, sizeof(vec));
    CHECK(N("sendmsg", client, page + 512, 0, 0, 0) == 7);
    vec[0] = (struct vector){page + 256, 2};
    vec[1] = (struct vector){page + 300, 5};
    gmem_write(&rt.mem, page + 640, vec, sizeof(vec));
    for (unsigned peek = 0; peek < 2; peek++) {
        CHECK(N("recvmsg", accepted, page + 512, peek ? 0 : 2, 0, 0) == 7);
        char out[7];
        gmem_read(&rt.mem, page + 256, out, 2);
        gmem_read(&rt.mem, page + 300, out + 2, 5);
        CHECK(!memcmp(out, "wake123", 7));
    }
    CHECK(N("recv", accepted, page + 256, 1, 0, 0) == -1 && error() == 35);
}
static volatile int receiver_started, receiver_done;
static int64_t receiver_result;
static bool receiver_select;
static int32_t receive_thread(void *arg) {
    PS5C_UNUSED(arg);
    __atomic_store_n(&receiver_started, 1, __ATOMIC_RELEASE);
    if (receiver_select) {
        uint8_t set[128] = {0};
        set[client / 8] = (uint8_t)(1u << (client & 7));
        gmem_write(&rt.mem, page + 2048, set, sizeof(set));
        receiver_result = N("select", client + 1, page + 2048, 0, 0, 0);
        if (receiver_result != 1) {
            __atomic_store_n(&receiver_done, 1, __ATOMIC_RELEASE);
            return 1;
        }
    }
    receiver_result = N("recv", client, page + 2000, 1, 0, 0);
    __atomic_store_n(&receiver_done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void blocking_receiver_wakes(void) {
    int zero = 0;
    gmem_write(&rt.mem, page + 64, &zero, 4);
    CHECK(N("setsockopt", client, 0xffff, 0x1200, page + 64, 4) == 0);
    pal_thread *worker = NULL;
    CHECK(pal_thread_create(&worker, receive_thread, NULL, "loopback receiver", 0) == PS5C_OK);
    while (!__atomic_load_n(&receiver_started, __ATOMIC_ACQUIRE))
        pal_thread_yield();
    pal_sleep_ns(10000000);
    CHECK(!__atomic_load_n(&receiver_done, __ATOMIC_ACQUIRE));
    gmem_write(&rt.mem, page + 128, "X", 1);
    CHECK(N("send", accepted, page + 128, 1, 0, 0) == 1);
    int32_t result;
    CHECK(pal_thread_join(worker, &result) == PS5C_OK && result == 0);
    char out = 0;
    gmem_read(&rt.mem, page + 2000, &out, 1);
    CHECK(receiver_result == 1 && out == 'X');
    nonblock(client);
}
static void select_readiness_and_wakeup(void) {
    uint8_t readset[128] = {0}, writeset[128] = {0}, out[128];
    int64_t timeout[2] = {0, 0};
    readset[client / 8] = (uint8_t)(1u << (client & 7));
    writeset[client / 8] = readset[client / 8];
    gmem_write(&rt.mem, page + 2048, readset, sizeof(readset));
    gmem_write(&rt.mem, page + 2176, writeset, sizeof(writeset));
    gmem_write(&rt.mem, page + 2304, timeout, sizeof(timeout));
    CHECK(N("select", client + 1, page + 2048, page + 2176, 0, page + 2304) == 1);
    gmem_read(&rt.mem, page + 2048, out, sizeof(out));
    for (unsigned i = 0; i < 128; i++)
        CHECK(out[i] == 0);
    gmem_read(&rt.mem, page + 2176, out, sizeof(out));
    CHECK(!memcmp(out, writeset, sizeof(out)));
    gmem_write(&rt.mem, page + 2048, readset, sizeof(readset));
    CHECK(N("select", client + 1, page + 2048, 0, 0, page + 2304) == 0);
    gmem_read(&rt.mem, page + 2048, out, sizeof(out));
    for (unsigned i = 0; i < 128; i++)
        CHECK(out[i] == 0);
    timeout[1] = 5000;
    gmem_write(&rt.mem, page + 2304, timeout, sizeof(timeout));
    gmem_write(&rt.mem, page + 2048, readset, sizeof(readset));
    uint64_t start = pal_time_ns();
    CHECK(N("select", client + 1, page + 2048, 0, 0, page + 2304) == 0 &&
          pal_time_ns() - start >= 5000000);
    memset(out, 0, sizeof(out));
    out[127] = 0x80;
    timeout[1] = 0;
    gmem_write(&rt.mem, page + 2048, out, sizeof(out));
    gmem_write(&rt.mem, page + 2304, timeout, sizeof(timeout));
    CHECK(N("select", 1024, page + 2048, 0, 0, page + 2304) == -1 && error() == 9);
    /* All pending read bits are cleared on timeout; a following send wakes
     * an actual blocking select, which must then permit recv. */
    receiver_started = receiver_done = 0;
    receiver_select = true;
    blocking_receiver_wakes();
    receiver_select = false;
    gmem_write(&rt.mem, page + 2048, readset, sizeof(readset));
    timeout[1] = -1;
    gmem_write(&rt.mem, page + 2304, timeout, sizeof(timeout));
    CHECK(N("select", client + 1, page + 2048, 0, 0, page + 2304) == -1 && error() == 22);
}
static void file_namespace_and_halfclose(void) {
    const char path[] = "/dev/urandom";
    gmem_write(&rt.mem, page + 128, path, sizeof(path));
    int fds[1024], count = 0;
    for (;;) {
        int fd = (int)N("open", page + 128, 0, 0, 0, 0);
        if (fd < 0)
            break;
        CHECK(count < 1024 && fd != client && fd != listener && fd != accepted);
        fds[count++] = fd;
    }
    CHECK(error() == 24 && count == 1018); /* 1024 slots, 3 stdio, 3 live sockets */
    for (int i = 0; i < count; i++)
        CHECK(N("close", fds[i], 0, 0, 0, 0) == 0);
    CHECK(N("shutdown", client, 1, 0, 0, 0) == 0);
    CHECK(N("recv", accepted, page + 256, 1, 0, 0) == 0);
    CHECK(N("send", client, page + 128, 1, 0, 0) == -1 && error() == 32);
    CHECK(N("send", accepted, page + 128, 1, 0, 0) == 1);
    CHECK(N("recv", client, page + 256, 1, 0, 0) == 1); /* peer's write half remains open */
    CHECK(N("close", accepted, 0, 0, 0, 0) == 0);
    CHECK(N("recv", client, page + 256, 1, 0, 0) == 0);
    CHECK(N("close", accepted, 0, 0, 0, 0) == -1 && error() == 9);
    CHECK(N("close", client, 0, 0, 0, 0) == 0 && N("close", listener, 0, 0, 0, 0) == 0);
}
static void pool_exhaustion_and_offline(void) {
    int ids[24];
    for (unsigned i = 0; i < 24; i++) {
        ids[i] = (int)N("socket", 2, 1, 0, 0, 0);
        CHECK(ids[i] >= 0);
        for (unsigned j = 0; j < i; j++)
            CHECK(ids[i] != ids[j]);
    }
    CHECK(N("socket", 2, 1, 0, 0, 0) == -1 && error() == 24);
    address(443, 203);
    CHECK(N("connect", ids[0], page, 16, 0, 0) == -1 && error() == 61);
    for (unsigned i = 0; i < 24; i++)
        CHECK(N("close", ids[i], 0, 0, 0, 0) == 0);
    int reused = (int)N("socket", 2, 1, 0, 0, 0);
    CHECK(reused >= 0);
    address(32918, 127);
    CHECK(N("connect", reused, page, 16, 0, 0) == -1 && error() == 61);
    CHECK(N("close", reused, 0, 0, 0, 0) == 0);
}
int main(void) {
    if (pal_init() != PS5C_OK || hle_registry_init() != PS5C_OK)
        return 2;
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    if (gmem_init(&rt.mem, &cfg) != PS5C_OK || gmem_map(&rt.mem, 0, 16 * GUEST_PAGE_SIZE, GPROT_RW,
                                                        0, "socket fixtures", &page) != PS5C_OK)
        return 2;
    thread.errno_addr = page + 1024;
    const test_case cases[] = {
        TEST_CASE(connect_local_channel),        TEST_CASE(bytes_and_readiness),
        TEST_CASE(stream_wrap_and_backpressure), TEST_CASE(vectored_peek_and_consumption),
        TEST_CASE(blocking_receiver_wakes),      TEST_CASE(select_readiness_and_wakeup),
        TEST_CASE(file_namespace_and_halfclose), TEST_CASE(pool_exhaustion_and_offline)};
    int result = test_run_all("net_loopback", cases, PS5C_ARRAY_COUNT(cases));
    gmem_free(&rt.mem);
    hle_registry_free();
    pal_shutdown();
    return result;
}
