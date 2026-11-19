/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceJson2 through its exports, as a game calls them: values built and
 * read back, objects and arrays filled and walked with iterators, text parsed
 * and serialized, strings returned by reference into guest memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle/hle.h"
#include "loader/nid.h"
#include "pal/pal.h"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x);                                        \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static gmem g_m;
static guest_addr g_heap, g_top;

static uint64_t call4(const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, double x0, double *ret_d) {
    const hle_export *e = hle_find("libSceJson2", nid_hash_name(name));
    if (!e || !e->fn) {
        fprintf(stderr, "missing export %s\n", name);
        exit(1);
    }
    hle_args args;
    memset(&args, 0, sizeof(args));
    args.gpr[0] = a0, args.gpr[1] = a1, args.gpr[2] = a2, args.gpr[3] = a3;
    memcpy(&args.xmm[0][0], &x0, 8);
    hle_call c = {&args, NULL, NULL, &g_m};
    e->fn(&c);
    if (ret_d) memcpy(ret_d, &args.ret_xmm[0][0], 8);
    return args.ret[0];
}
#define CALL(name, ...) call4(name, __VA_ARGS__, 0, NULL)
static uint64_t call(const char *name, uint64_t a0) { return call4(name, a0, 0, 0, 0, 0, NULL); }
static uint64_t call2(const char *name, uint64_t a0, uint64_t a1) { return call4(name, a0, a1, 0, 0, 0, NULL); }

/* The guest's compiled callers load through RAX: byte, qword or double.
 * Verify that exported getters return mapped references, not register values. */
static uint64_t reference_bits(const char *name,guest_addr value,uint64_t size) {
    guest_addr address=call(name,value);uint64_t bits=0;
    if(!address || !gmem_read(&g_m,address,&bits,size)) {
        fprintf(stderr,"invalid primitive reference from %s: 0x%llx\n",name,(unsigned long long)address);
        exit(1);
    }
    return bits;
}

/* guest memory: 8-byte objects and C strings */
static guest_addr obj(void) {
    guest_addr a = g_top;
    g_top += 16;
    return a;
}
static guest_addr cstr(const char *s) {
    guest_addr a = g_top;
    gmem_write(&g_m, a, s, strlen(s) + 1);
    g_top += (strlen(s) + 16) & ~7ull;
    return a;
}
static const char *host_str(guest_addr a) { return a ? (const char *)gmem_host(&g_m, a, 1) : ""; }

/* serialize(Value) as host text */
static const char *text_of(guest_addr value) {
    static char buf[1024];
    guest_addr s = obj();
    call("_ZN3sce4Json6StringC1Ev", s);
    call2("_ZN3sce4Json5Value9serializeERNS0_6StringE", value, s);
    snprintf(buf, sizeof(buf), "%s", host_str(call("_ZNK3sce4Json6String5c_strEv", s)));
    call("_ZN3sce4Json6StringD1Ev", s);
    return buf;
}

int main(void) {
    CHECK(pal_init() == PS5C_OK);
    gmem_config cfg = gmem_default_config();
    cfg.identity = false;
    CHECK(gmem_init(&g_m, &cfg) == PS5C_OK);
    CHECK(gmem_map(&g_m, 0, 16 * GUEST_PAGE_SIZE, GPROT_RW, 0, "test", &g_heap) == PS5C_OK);
    g_top = g_heap;
    CHECK(hle_registry_init() == PS5C_OK);

    /* a string value, read back by reference */
    guest_addr v = obj();
    call2("_ZN3sce4Json5ValueC1EPKc", v, cstr("caf\xc3\xa9 \"x\""));
    CHECK(call("_ZNK3sce4Json5Value7getTypeEv", v) == 5);
    guest_addr s = call("_ZNK3sce4Json5Value9getStringEv", v);
    CHECK(!strcmp(host_str(call("_ZNK3sce4Json6String5c_strEv", s)), "caf\xc3\xa9 \"x\""));
    CHECK(call("_ZNK3sce4Json6String6lengthEv", s) == 9);
    CHECK(!strcmp(text_of(v), "\"caf\xc3\xa9 \\\"x\\\"\""));

    /* numbers */
    call2("_ZN3sce4Json5Value3setEl", v, (uint64_t)-42);
    CHECK((int64_t)reference_bits("_ZNK3sce4Json5Value10getIntegerEv", v,8) == -42);
    guest_addr integer_ref=call("_ZNK3sce4Json5Value10getIntegerEv",v);
    CHECK(integer_ref==call("_ZNK3sce4Json5Value10getIntegerEv",v));
    uint64_t converted=reference_bits("_ZNK3sce4Json5Value7getRealEv",v,8);
    double integer_as_real;memcpy(&integer_as_real,&converted,8);CHECK(integer_as_real==-42.0);
    CHECK(!strcmp(text_of(v),"-42"));
    call2("_ZN3sce4Json5Value3setEb",v,1);
    CHECK(reference_bits("_ZNK3sce4Json5Value10getBooleanEv",v,1)==1);
    call2("_ZN3sce4Json5Value3setEb",v,0);
    CHECK(reference_bits("_ZNK3sce4Json5Value10getBooleanEv",v,1)==0);
    double d = 0;
    call4("_ZN3sce4Json5Value3setEd", v, 0, 0, 0, 2.5, NULL);
    uint64_t real_bits=reference_bits("_ZNK3sce4Json5Value7getRealEv",v,8);
    memcpy(&d,&real_bits,8);
    CHECK(d == 2.5);
    guest_addr real_ref=call("_ZNK3sce4Json5Value7getRealEv",v);
    CHECK(real_ref==call("_ZNK3sce4Json5Value7getRealEv",v));
    CHECK(!strcmp(text_of(v), "2.5"));

    /* an object: o["name"] = 42 through operator= on the returned reference */
    guest_addr o = obj(), key = obj(), n42 = obj();
    call("_ZN3sce4Json6ObjectC1Ev", o);
    call2("_ZN3sce4Json6StringC1EPKc", key, cstr("name"));
    call2("_ZN3sce4Json5ValueC1El", n42, 42);
    guest_addr slot = call2("_ZN3sce4Json6ObjectixERKNS0_6StringE", o, key);
    CHECK(slot);
    call2("_ZN3sce4Json5ValueaSERKS1_", slot, n42);
    CHECK(call("_ZNK3sce4Json6Object4sizeEv", o) == 1);
    guest_addr root = obj();
    call2("_ZN3sce4Json5ValueC1ERKNS0_6ObjectE", root, o);
    CHECK(!strcmp(text_of(root), "{\"name\":42}"));
    /* the object was copied: changing it leaves the value as it was */
    call2("_ZN3sce4Json5Value3setEb", n42, 1);
    call2("_ZN3sce4Json5ValueaSERKS1_", slot, n42);
    CHECK(!strcmp(text_of(root), "{\"name\":42}"));

    /* parse, then walk */
    const char *json = "{\"a\":[1,2.5,\"x\",true,null],\"b\":{\"c\":-3},\"big\":18446744073709551615}";
    guest_addr p = obj();
    call("_ZN3sce4Json5ValueC1Ev", p);
    CHECK(call4("_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", p, cstr(json), strlen(json), 0, 0, NULL) == 0);
    CHECK(!strcmp(text_of(p), json));
    CHECK(call("_ZNK3sce4Json5Value5countEv", p) == 3);
    guest_addr a = call2("_ZNK3sce4Json5ValueixEPKc", p, cstr("a"));
    CHECK(call("_ZNK3sce4Json5Value5countEv", a) == 5);
    guest_addr e2 = call2("_ZNK3sce4Json5ValueixEm", a, 2);
    CHECK(!strcmp(host_str(call("_ZNK3sce4Json6String5c_strEv", call("_ZNK3sce4Json5Value9getStringEv", e2))), "x"));
    guest_addr big = call2("_ZNK3sce4Json5ValueixEPKc", p, cstr("big"));
    CHECK(call("_ZNK3sce4Json5Value7getTypeEv", big) == 3 && reference_bits("_ZNK3sce4Json5Value11getUIntegerEv", big,8) == ~0ull);
    /* a missing member: a null default, not a crash */
    guest_addr miss = call2("_ZNK3sce4Json5ValueixEPKc", p, cstr("nope"));
    CHECK(miss && call("_ZNK3sce4Json5Value7getTypeEv", miss) == 0);
    /* array iteration: begin / end through the hidden result pointer */
    guest_addr arr = call("_ZNK3sce4Json5Value8getArrayEv", a), it = obj(), end = obj();
    call2("_ZNK3sce4Json5Array5beginEv", it, arr);
    call2("_ZNK3sce4Json5Array3endEv", end, arr);
    uint32_t count = 0;
    while (call2("_ZNK3sce4Json5Array8iteratorneERKS2_", it, end)) {
        CHECK(call("_ZNK3sce4Json5Array8iteratordeEv", it));
        call("_ZN3sce4Json5Array8iteratorppEv", it);
        count++;
    }
    CHECK(count == 5);
    /* object iteration: the Pair's value at +16 */
    guest_addr b = call("_ZNK3sce4Json5Value9getObjectEv", call2("_ZNK3sce4Json5ValueixEPKc", p, cstr("b")));
    call2("_ZNK3sce4Json6Object5beginEv", it, b);
    guest_addr pair = call("_ZNK3sce4Json6Object8iteratordeEv", it);
    CHECK(!strcmp(host_str(call("_ZNK3sce4Json6String5c_strEv", pair)), "c"));
    CHECK((int64_t)reference_bits("_ZNK3sce4Json5Value10getIntegerEv", pair + 16,8) == -3);
    /* bad text: an error, the value unchanged */
    CHECK((int32_t)call4("_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", p, cstr("{\"a\":"), 5, 0, 0, NULL) ==
          (int32_t)0x80920101);
    CHECK(call("_ZNK3sce4Json5Value5countEv", p) == 3);

    /* push_back copies */
    guest_addr arr2 = obj(), item = obj(), wrap = obj();
    call("_ZN3sce4Json5ArrayC1Ev", arr2);
    call2("_ZN3sce4Json5ValueC1EPKc", item, cstr("q"));
    call2("_ZN3sce4Json5Array9push_backERKNS0_5ValueE", arr2, item);
    call2("_ZN3sce4Json5Array9push_backERKNS0_5ValueE", arr2, n42);
    call2("_ZN3sce4Json5ValueC1ERKNS0_5ArrayE", wrap, arr2);
    CHECK(!strcmp(text_of(wrap), "[\"q\",true]"));

    /* everything freed and the arena reused */
    const char *dtors[] = {"_ZN3sce4Json5ValueD1Ev"};
    guest_addr values[] = {v, n42, root, p, item, wrap};
    for (size_t i = 0; i < PS5C_ARRAY_COUNT(values); i++) call(dtors[0], values[i]);
    call("_ZN3sce4Json6ObjectD1Ev", o);
    call("_ZN3sce4Json5ArrayD1Ev", arr2);
    call("_ZN3sce4Json6StringD1Ev", key);
    hle_registry_free();
    gmem_free(&g_m);
    pal_shutdown();
    puts("Json2: values, objects, arrays, iterators, parse and serialize");
    return 0;
}
