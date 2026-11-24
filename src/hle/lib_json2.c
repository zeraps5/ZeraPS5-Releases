/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceJson2: the SDK's JSON values (sce::Json::Value, String, Array, Object,
 * Parser). Games embed these 8-byte objects in their own structures and get
 * references into them (operator[], getString, c_str, iterators), so all the
 * data lives in guest memory, in an arena this library manages:
 *
 *   Value   u64 node
 *   node    u32 type (+0), u8 boolean (+4), i64 integer (+8), u64 uinteger (+16),
 *           double real (+24), String (+32), Array (+40), Object (+48)
 *   String  u64 text: { u64 length; char chars[length + 1] }, 0 when empty
 *   Array   u64 list: { u64 count, head, tail } of cells { u64 next; Value }
 *   Object  u64 list of cells { u64 next; Pair { String key; u64 reserved; Value value } }
 *   iterators: u64 cell (0 at the end); an Object iterator dereferences to the Pair
 *
 * Iterators are returned by value through a hidden result pointer (they have
 * a destructor). Errors: parse 0x80920101. Accesses of a missing member or of
 * the wrong type go to the game's null access callback, else to a default
 * value of the type. The game's allocator (InitParameter2) is not used.
 *
 * Which functions games call and the InitParameter2 and Pair layouts were
 * checked against the AnyPS5 project's libSceJson2 (boykopovar/AnyPS5,
 * GPL-2.0); this is our own implementation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/log.h"
#include "core/runtime.h"
#include "hle/hle.h"
#include "pal/pal.h"

void hle_register_table(const hle_export *table, size_t count);

enum { T_NULL, T_BOOL, T_INT, T_UINT, T_REAL, T_STRING, T_ARRAY, T_OBJECT };
#define JSON_ERROR_PARSE ((int32_t)0x80920101)
#define NODE_SIZE 56u
#define N_TYPE 0
#define N_BOOL 4
#define N_INT 8
#define N_UINT 16
#define N_REAL 24
#define N_STRING 32
#define N_ARRAY 40
#define N_OBJECT 48

static volatile int32_t g_lock_flag;
static void jlock(void) { while (__atomic_test_and_set(&g_lock_flag, __ATOMIC_ACQUIRE)) pal_sleep_ns(0); }
static void junlock(void) { __atomic_clear(&g_lock_flag, __ATOMIC_RELEASE); }

/* ---- Guest arena: size classes of 16 << k bytes with a 16-byte header (class, magic);
 * free blocks link through their first 8 bytes. Larger blocks get their own pages. ---- */

#define ARENA_CHUNK 0x10000u
#define CLASSES 9 /* 16 .. 4096 */
#define BLOCK_MAGIC 0x4A534F4Eu
static gmem *g_mem;
static guest_addr g_free[CLASSES], g_chunk, g_chunk_end;

static uint64_t rd64(guest_addr a) {
    uint64_t v = 0;
    if (a) gmem_read(g_mem, a, &v, 8);
    return v;
}
static void wr64(guest_addr a, uint64_t v) {
    if (a) gmem_write(g_mem, a, &v, 8);
}
static uint32_t rd32(guest_addr a) {
    uint32_t v = 0;
    if (a) gmem_read(g_mem, a, &v, 4);
    return v;
}
static void wr32(guest_addr a, uint32_t v) {
    if (a) gmem_write(g_mem, a, &v, 4);
}

static guest_addr jalloc(uint64_t size) {
    uint32_t k = 0;
    while (k < CLASSES && (16ull << k) < size) k++;
    guest_addr block = 0;
    if (k == CLASSES) {
        uint64_t bytes = PS5C_ALIGN_UP(size + 16, GUEST_PAGE_SIZE);
        guest_addr a = 0;
        if (gmem_map(g_mem, 0, bytes, GPROT_RW, 0, "json2 block", &a) != PS5C_OK) return 0;
        uint32_t hdr[4] = {CLASSES, BLOCK_MAGIC, (uint32_t)bytes, (uint32_t)(bytes >> 32)};
        gmem_write(g_mem, a, hdr, sizeof(hdr));
        return a + 16;
    }
    uint64_t total = (16ull << k) + 16;
    if (g_free[k]) {
        block = g_free[k];
        g_free[k] = rd64(block);
    } else {
        if (g_chunk + total > g_chunk_end) {
            guest_addr a = 0;
            if (gmem_map(g_mem, 0, ARENA_CHUNK, GPROT_RW, 0, "json2 arena", &a) != PS5C_OK) return 0;
            g_chunk = a, g_chunk_end = a + ARENA_CHUNK;
        }
        uint32_t hdr[4] = {k, BLOCK_MAGIC, 0, 0};
        gmem_write(g_mem, g_chunk, hdr, sizeof(hdr));
        block = g_chunk + 16;
        g_chunk += total;
    }
    void *p = gmem_host(g_mem, block, 16ull << k);
    if (p) memset(p, 0, 16ull << k);
    return block;
}

static void jfree(guest_addr block) {
    uint32_t hdr[4] = {0};
    if (!block || !gmem_read(g_mem, block - 16, hdr, sizeof(hdr)) || hdr[1] != BLOCK_MAGIC) return;
    if (hdr[0] == CLASSES) {
        gmem_unmap(g_mem, block - 16, (uint64_t)hdr[2] | ((uint64_t)hdr[3] << 32));
        return;
    }
    if (hdr[0] < CLASSES) {
        wr64(block, g_free[hdr[0]]);
        g_free[hdr[0]] = block;
    }
}

/* ---- Strings ---- */

/* The text of String object `s` (a NUL-terminated host copy the caller frees) */
static char *str_get(guest_addr s, uint64_t *len_out) {
    guest_addr text = rd64(s);
    uint64_t len = rd64(text);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    if (len && !gmem_read(g_mem, text + 8, out, len)) len = 0;
    out[len] = 0;
    if (len_out) *len_out = len;
    return out;
}

static void str_set(guest_addr s, const char *text, uint64_t len) {
    if (!s) return;
    jfree(rd64(s));
    guest_addr buf = 0;
    if (len) {
        buf = jalloc(len + 9);
        if (buf) {
            wr64(buf, len);
            gmem_write(g_mem, buf + 8, text, len);
        }
    }
    wr64(s, buf);
}

static void str_clear(guest_addr s) {
    jfree(rd64(s));
    wr64(s, 0);
}

static bool str_equals(guest_addr s, const char *text, uint64_t len) {
    guest_addr t = rd64(s);
    if (rd64(t) != len) return false;
    if (!len) return true;
    const char *p = (const char *)gmem_host(g_mem, t + 8, len);
    return p && !memcmp(p, text, len);
}

/* ---- Nodes, Values, containers ---- */

static void node_clear(guest_addr n);
static guest_addr node_copy_new(guest_addr src);

static guest_addr node_new(uint32_t type) {
    guest_addr n = jalloc(NODE_SIZE);
    wr32(n + N_TYPE, type);
    return n;
}
static void node_free(guest_addr n) {
    if (!n) return;
    node_clear(n);
    jfree(n);
}

/* The node of Value `v`, created (null) for a Value no constructor ran on */
static guest_addr value_node(guest_addr v) {
    guest_addr n = rd64(v);
    if (!n && v) {
        n = node_new(T_NULL);
        wr64(v, n);
    }
    return n;
}

static void list_free_array(guest_addr list) {
    for (guest_addr cell = rd64(list + 8); cell;) {
        guest_addr next = rd64(cell);
        node_free(rd64(cell + 8));
        jfree(cell);
        cell = next;
    }
    jfree(list);
}
static void list_free_object(guest_addr list) {
    for (guest_addr cell = rd64(list + 8); cell;) {
        guest_addr next = rd64(cell);
        str_clear(cell + 8);
        node_free(rd64(cell + 24));
        jfree(cell);
        cell = next;
    }
    jfree(list);
}

static void array_clear(guest_addr a) {
    guest_addr list = rd64(a);
    if (list) list_free_array(list);
    wr64(a, 0);
}
static void object_clear(guest_addr o) {
    guest_addr list = rd64(o);
    if (list) list_free_object(list);
    wr64(o, 0);
}

static void node_clear(guest_addr n) {
    if (!n) return;
    str_clear(n + N_STRING);
    array_clear(n + N_ARRAY);
    object_clear(n + N_OBJECT);
    uint8_t zero[NODE_SIZE] = {0};
    gmem_write(g_mem, n, zero, NODE_SIZE);
}

static guest_addr list_of(guest_addr container) {
    guest_addr list = rd64(container);
    if (!list && container) {
        list = jalloc(24);
        wr64(container, list);
    }
    return list;
}

/* Appends a cell of `cell_size` bytes; returns it */
static guest_addr list_append(guest_addr container, uint64_t cell_size) {
    guest_addr list = list_of(container), cell = jalloc(cell_size);
    if (!list || !cell) return 0;
    guest_addr tail = rd64(list + 16);
    if (tail) wr64(tail, cell);
    else wr64(list + 8, cell);
    wr64(list + 16, cell);
    wr64(list, rd64(list) + 1);
    return cell;
}

static void array_push_copy(guest_addr a, guest_addr src_node) {
    guest_addr cell = list_append(a, 16);
    if (cell) wr64(cell + 8, node_copy_new(src_node));
}

/* The Pair of `key` in Object `o`, 0 if absent */
static guest_addr object_find(guest_addr o, const char *key, uint64_t len) {
    guest_addr list = rd64(o);
    for (guest_addr cell = list ? rd64(list + 8) : 0; cell; cell = rd64(cell))
        if (str_equals(cell + 8, key, len)) return cell + 8;
    return 0;
}
static guest_addr object_insert(guest_addr o, const char *key, uint64_t len) {
    guest_addr pair = object_find(o, key, len);
    if (pair) return pair;
    guest_addr cell = list_append(o, 32);
    if (!cell) return 0;
    str_set(cell + 8, key, len);
    wr64(cell + 24, node_new(T_NULL));
    return cell + 8;
}

static void array_copy(guest_addr dst, guest_addr src) {
    array_clear(dst);
    guest_addr list = rd64(src);
    for (guest_addr cell = list ? rd64(list + 8) : 0; cell; cell = rd64(cell)) array_push_copy(dst, rd64(cell + 8));
    if (!rd64(dst)) list_of(dst);
}
static void object_copy(guest_addr dst, guest_addr src) {
    object_clear(dst);
    guest_addr list = rd64(src);
    for (guest_addr cell = list ? rd64(list + 8) : 0; cell; cell = rd64(cell)) {
        uint64_t len = 0;
        char *key = str_get(cell + 8, &len);
        guest_addr dcell = list_append(dst, 32);
        if (dcell && key) {
            str_set(dcell + 8, key, len);
            wr64(dcell + 24, node_copy_new(rd64(cell + 24)));
        }
        free(key);
    }
    if (!rd64(dst)) list_of(dst);
}

/* dst = src (both nodes), deep */
static void node_assign(guest_addr dst, guest_addr src) {
    if (!dst || dst == src) return;
    uint32_t type = src ? rd32(src + N_TYPE) : T_NULL;
    uint8_t scalars[N_STRING] = {0};
    if (src) gmem_read(g_mem, src, scalars, sizeof(scalars));
    node_clear(dst);
    gmem_write(g_mem, dst, scalars, sizeof(scalars));
    if (type == T_STRING) {
        uint64_t len = 0;
        char *s = str_get(src + N_STRING, &len);
        if (s) str_set(dst + N_STRING, s, len);
        free(s);
    } else if (type == T_ARRAY) {
        array_copy(dst + N_ARRAY, src + N_ARRAY);
    } else if (type == T_OBJECT) {
        object_copy(dst + N_OBJECT, src + N_OBJECT);
    }
}
static guest_addr node_copy_new(guest_addr src) {
    guest_addr n = node_new(T_NULL);
    node_assign(n, src);
    return n;
}

/* Changes the type of a node, its content emptied (containers present) */
static void node_set_type(guest_addr n, uint32_t type) {
    node_clear(n);
    wr32(n + N_TYPE, type);
    if (type == T_ARRAY) list_of(n + N_ARRAY);
    if (type == T_OBJECT) list_of(n + N_OBJECT);
}

/* ---- Null access: the game's callback, else a default value of the type ---- */

static guest_addr g_defaults[8], g_null_cb, g_null_ctx;

/* Value::setNullAccessCallBack: callbacks of single Values (by the Value's address), taken
 * before the global one for accesses through that Value */
#define VALUE_CALLBACKS 32
static struct {
    guest_addr value, cb, ctx;
} g_value_cb[VALUE_CALLBACKS];

static guest_addr default_value(uint32_t type) {
    if (type > 7) type = T_NULL;
    if (!g_defaults[type]) {
        guest_addr v = jalloc(8);
        guest_addr n = node_new(T_NULL);
        node_set_type(n, type);
        wr64(v, n);
        g_defaults[type] = v;
    }
    return g_defaults[type];
}

/* The Value to return when `parent` has no `type` value to give (lock held, released for the call) */
static guest_addr null_access(hle_call *c, uint32_t type, guest_addr parent) {
    guest_addr cb = g_null_cb, ctx = g_null_ctx;
    for (int i = 0; parent && i < VALUE_CALLBACKS; i++)
        if (g_value_cb[i].value == parent) {
            cb = g_value_cb[i].cb, ctx = g_value_cb[i].ctx;
            break;
        }
    if (cb && c->rt) {
        junlock();
        uint64_t rax = 0;
        bool ok = runtime_call_nested(c->rt, cb, type, parent, ctx, &rax);
        jlock();
        if (ok && rax) {
            guest_addr n = rd64(rax);
            if (n && (type == T_NULL || rd32(n + N_TYPE) == type)) return rax;
        }
    }
    return default_value(type);
}

/* The node of Value `v` when it has `type`, else the null access result's node */
static guest_addr typed_node(hle_call *c, guest_addr v, uint32_t type) {
    guest_addr n = value_node(v);
    if (n && rd32(n + N_TYPE) == type) return n;
    return value_node(null_access(c, type, v));
}

/* ---- Serialization ---- */

typedef struct sbuf {
    char *p;
    size_t len, cap;
} sbuf;
static void sb_put(sbuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = (char *)realloc(b->p, cap);
        if (!p) return;
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void sb_str(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static void sb_escaped(sbuf *b, guest_addr s) {
    uint64_t len = 0;
    char *t = str_get(s, &len);
    sb_put(b, "\"", 1);
    for (uint64_t i = 0; t && i < len; i++) {
        unsigned char ch = (unsigned char)t[i];
        char esc[8];
        switch (ch) {
        case '"': sb_str(b, "\\\""); break;
        case '\\': sb_str(b, "\\\\"); break;
        case '\b': sb_str(b, "\\b"); break;
        case '\f': sb_str(b, "\\f"); break;
        case '\n': sb_str(b, "\\n"); break;
        case '\r': sb_str(b, "\\r"); break;
        case '\t': sb_str(b, "\\t"); break;
        default:
            if (ch < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", ch);
                sb_str(b, esc);
            } else {
                sb_put(b, (const char *)&ch, 1);
            }
        }
    }
    sb_put(b, "\"", 1);
    free(t);
}

static void serialize(sbuf *b, guest_addr n, uint32_t depth) {
    char num[40];
    uint32_t type = n ? rd32(n + N_TYPE) : T_NULL;
    if (depth > 512) type = T_NULL;
    switch (type) {
    case T_BOOL: {
        uint8_t v = 0;
        gmem_read(g_mem, n + N_BOOL, &v, 1);
        sb_str(b, v ? "true" : "false");
        break;
    }
    case T_INT: snprintf(num, sizeof(num), "%lld", (long long)rd64(n + N_INT)); sb_str(b, num); break;
    case T_UINT: snprintf(num, sizeof(num), "%llu", (unsigned long long)rd64(n + N_UINT)); sb_str(b, num); break;
    case T_REAL: {
        uint64_t bits = rd64(n + N_REAL);
        double d;
        memcpy(&d, &bits, 8);
        if (!isfinite(d)) d = 0; /* JSON has no infinities or NaNs */
        snprintf(num, sizeof(num), "%.17g", d);
        sb_str(b, num);
        break;
    }
    case T_STRING: sb_escaped(b, n + N_STRING); break;
    case T_ARRAY: {
        sb_put(b, "[", 1);
        guest_addr list = rd64(n + N_ARRAY);
        bool first = true;
        for (guest_addr cell = list ? rd64(list + 8) : 0; cell; cell = rd64(cell)) {
            if (!first) sb_put(b, ",", 1);
            first = false;
            serialize(b, rd64(cell + 8), depth + 1);
        }
        sb_put(b, "]", 1);
        break;
    }
    case T_OBJECT: {
        sb_put(b, "{", 1);
        guest_addr list = rd64(n + N_OBJECT);
        bool first = true;
        for (guest_addr cell = list ? rd64(list + 8) : 0; cell; cell = rd64(cell)) {
            if (!first) sb_put(b, ",", 1);
            first = false;
            sb_escaped(b, cell + 8);
            sb_put(b, ":", 1);
            serialize(b, rd64(cell + 24), depth + 1);
        }
        sb_put(b, "}", 1);
        break;
    }
    default: sb_str(b, "null"); break;
    }
}

/* ---- Parser: text into a node ---- */

typedef struct jparser {
    const char *p, *end;
    uint32_t depth;
} jparser;

static void jp_skip(jparser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}
static bool jp_lit(jparser *j, const char *w) {
    size_t n = strlen(w);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, w, n)) return false;
    j->p += n;
    return true;
}
static void utf8(sbuf *b, uint32_t c) {
    char o[4];
    size_t n;
    if (c < 0x80) o[0] = (char)c, n = 1;
    else if (c < 0x800) o[0] = (char)(0xC0 | (c >> 6)), o[1] = (char)(0x80 | (c & 0x3F)), n = 2;
    else if (c < 0x10000) o[0] = (char)(0xE0 | (c >> 12)), o[1] = (char)(0x80 | ((c >> 6) & 0x3F)), o[2] = (char)(0x80 | (c & 0x3F)), n = 3;
    else o[0] = (char)(0xF0 | (c >> 18)), o[1] = (char)(0x80 | ((c >> 12) & 0x3F)), o[2] = (char)(0x80 | ((c >> 6) & 0x3F)), o[3] = (char)(0x80 | (c & 0x3F)), n = 4;
    sb_put(b, o, n);
}
static bool jp_hex4(jparser *j, uint32_t *code) {
    if (j->end - j->p < 4) return false;
    *code = 0;
    for (int i = 0; i < 4; i++) {
        char ch = *j->p++;
        *code <<= 4;
        if (ch >= '0' && ch <= '9') *code |= (uint32_t)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') *code |= (uint32_t)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') *code |= (uint32_t)(ch - 'A' + 10);
        else return false;
    }
    return true;
}
/* A quoted string (the opening quote at j->p) into b */
static bool jp_string(jparser *j, sbuf *b) {
    if (j->p >= j->end || *j->p != '"') return false;
    j->p++;
    sb_put(b, "", 0);
    while (j->p < j->end && *j->p != '"') {
        char ch = *j->p++;
        if ((unsigned char)ch < 0x20) return false;
        if (ch != '\\') {
            sb_put(b, &ch, 1);
            continue;
        }
        if (j->p >= j->end) return false;
        ch = *j->p++;
        uint32_t code;
        switch (ch) {
        case '"': case '\\': case '/': sb_put(b, &ch, 1); break;
        case 'b': sb_put(b, "\b", 1); break;
        case 'f': sb_put(b, "\f", 1); break;
        case 'n': sb_put(b, "\n", 1); break;
        case 'r': sb_put(b, "\r", 1); break;
        case 't': sb_put(b, "\t", 1); break;
        case 'u':
            if (!jp_hex4(j, &code)) return false;
            if (code >= 0xD800 && code < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                uint32_t low;
                j->p += 2;
                if (!jp_hex4(j, &low) || low < 0xDC00 || low >= 0xE000) return false;
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            }
            utf8(b, code);
            break;
        default: return false;
        }
    }
    if (j->p >= j->end) return false;
    j->p++;
    return true;
}

static bool jp_value(jparser *j, guest_addr n);

static bool jp_number(jparser *j, guest_addr n) {
    const char *s = j->p;
    bool real = false;
    if (j->p < j->end && *j->p == '-') j->p++;
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return false;
    while (j->p < j->end && ((*j->p >= '0' && *j->p <= '9') || *j->p == '.' || *j->p == 'e' || *j->p == 'E' ||
                             *j->p == '+' || *j->p == '-')) {
        if (*j->p == '.' || *j->p == 'e' || *j->p == 'E') real = true;
        j->p++;
    }
    char tmp[64];
    size_t len = (size_t)(j->p - s);
    if (len >= sizeof(tmp)) return false;
    memcpy(tmp, s, len);
    tmp[len] = 0;
    if (real) {
        double d = strtod(tmp, NULL);
        uint64_t bits;
        memcpy(&bits, &d, 8);
        wr32(n + N_TYPE, T_REAL);
        wr64(n + N_REAL, bits);
    } else if (tmp[0] == '-') {
        wr32(n + N_TYPE, T_INT);
        wr64(n + N_INT, (uint64_t)strtoll(tmp, NULL, 10));
    } else {
        unsigned long long u = strtoull(tmp, NULL, 10);
        if (u <= 0x7FFFFFFFFFFFFFFFull) {
            wr32(n + N_TYPE, T_INT);
            wr64(n + N_INT, u);
        } else {
            wr32(n + N_TYPE, T_UINT);
            wr64(n + N_UINT, u);
        }
    }
    return true;
}

static bool jp_value(jparser *j, guest_addr n) {
    jp_skip(j);
    if (j->p >= j->end || ++j->depth > 512) return false;
    bool ok = true;
    char ch = *j->p;
    if (ch == '{') {
        node_set_type(n, T_OBJECT);
        j->p++;
        jp_skip(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
        } else {
            for (;;) {
                sbuf key = {0};
                jp_skip(j);
                ok = jp_string(j, &key);
                jp_skip(j);
                ok = ok && j->p < j->end && *j->p++ == ':';
                guest_addr pair = ok ? object_insert(n + N_OBJECT, key.p ? key.p : "", key.len) : 0;
                free(key.p);
                ok = ok && pair && jp_value(j, rd64(pair + 16));
                jp_skip(j);
                if (!ok || j->p >= j->end) {
                    ok = false;
                    break;
                }
                if (*j->p == ',') {
                    j->p++;
                    continue;
                }
                ok = *j->p++ == '}';
                break;
            }
        }
    } else if (ch == '[') {
        node_set_type(n, T_ARRAY);
        j->p++;
        jp_skip(j);
        if (j->p < j->end && *j->p == ']') {
            j->p++;
        } else {
            for (;;) {
                guest_addr cell = list_append(n + N_ARRAY, 16), item = node_new(T_NULL);
                wr64(cell + 8, item);
                ok = cell && item && jp_value(j, item);
                jp_skip(j);
                if (!ok || j->p >= j->end) {
                    ok = false;
                    break;
                }
                if (*j->p == ',') {
                    j->p++;
                    continue;
                }
                ok = *j->p++ == ']';
                break;
            }
        }
    } else if (ch == '"') {
        sbuf s = {0};
        ok = jp_string(j, &s);
        if (ok) {
            node_set_type(n, T_STRING);
            str_set(n + N_STRING, s.p ? s.p : "", s.len);
        }
        free(s.p);
    } else if (jp_lit(j, "true") || jp_lit(j, "false")) {
        uint8_t v = j->p[-1] == 'e' && j->p[-2] == 'u';
        node_set_type(n, T_BOOL);
        gmem_write(g_mem, n + N_BOOL, &v, 1);
    } else if (jp_lit(j, "null")) {
        node_set_type(n, T_NULL);
    } else {
        node_clear(n);
        ok = jp_number(j, n);
    }
    j->depth--;
    return ok;
}

/* Parses text into Value `out` (unchanged on an error) */
static int32_t parse_into(guest_addr out, const char *text, size_t size) {
    guest_addr n = node_new(T_NULL);
    jparser j = {text, text + size, 0};
    bool ok = n && jp_value(&j, n);
    jp_skip(&j);
    ok = ok && (j.p == j.end || *j.p == 0);
    if (!ok) {
        node_free(n);
        return JSON_ERROR_PARSE;
    }
    guest_addr old = rd64(out);
    wr64(out, n);
    node_free(old);
    return 0;
}

/* ---- Exports ---- */

#define BEGIN(c)                                                                                                       \
    jlock();                                                                                                           \
    g_mem = (c)->mem
#define END() junlock()

static void ret_zero(hle_call *c) { hle_ret_u64(c, 0); }

static double arg_double(hle_call *c) {
    double d;
    memcpy(&d, &c->a->xmm[0][0], 8);
    return d;
}

/* Initializer / InitParameter2 { allocator, user data, file buffer size, special float format (+0x18) } */
/* int Value::setNullAccessCallBack(callback, void *context) */
static void value_set_null_callback(hle_call *c) {
    guest_addr v = hle_arg_ptr(c, 0), cb = hle_arg_ptr(c, 1), ctx = hle_arg_ptr(c, 2);
    int slot = -1;
    jlock();
    for (int i = 0; i < VALUE_CALLBACKS; i++) {
        if (g_value_cb[i].value == v) { slot = i; break; }
        if (slot < 0 && !g_value_cb[i].value) slot = i;
    }
    if (slot >= 0) {
        g_value_cb[slot].value = cb ? v : 0;
        g_value_cb[slot].cb = cb;
        g_value_cb[slot].ctx = ctx;
    }
    junlock();
    static int logged;
    if (slot < 0 && logged++ < 2) LOG_W(HLE, "Json2: more than %d values with a null access callback", VALUE_CALLBACKS);
    hle_ret_s32(c, 0);
}

/* int Value::setSpecialFloatHandler(handler, void *context): NaN and infinities are written in the
 * InitParameter2 format (setSpecialFloatFormatType); the handler is not called */
static void value_set_special_float_handler(hle_call *c) { hle_ret_s32(c, 0); }

static void initializer_set_null_callback(hle_call *c) {
    BEGIN(c);
    g_null_cb = hle_arg_ptr(c, 1);
    g_null_ctx = hle_arg_ptr(c, 2);
    END();
    hle_ret_s32(c, 0);
}
static void init_param2_ctor(hle_call *c) {
    void *p = hle_mem(c, hle_arg_ptr(c, 0), 0x28);
    if (p) memset(p, 0, 0x28);
    hle_ret_u64(c, 0);
}
static void init_param2_set_allocator(hle_call *c) {
    uint64_t v[2] = {hle_arg(c, 1), hle_arg(c, 2)};
    gmem_write(c->mem, hle_arg_ptr(c, 0), v, sizeof(v));
    hle_ret_u64(c, 0);
}
static void init_param2_set_file_buffer_size(hle_call *c) {
    uint64_t v = hle_arg(c, 1);
    gmem_write(c->mem, hle_arg_ptr(c, 0) + 16, &v, 8);
    hle_ret_u64(c, 0);
}
static void init_param2_set_float_format(hle_call *c) {
    uint32_t v = hle_arg_u32(c, 1);
    gmem_write(c->mem, hle_arg_ptr(c, 0) + 0x18, &v, 4);
    hle_ret_u64(c, 0);
}
static void init_param_rtti_ctor(hle_call *c) {
    uint64_t v[3] = {hle_arg(c, 1), hle_arg(c, 2), hle_arg(c, 3)};
    gmem_write(c->mem, hle_arg_ptr(c, 0), v, sizeof(v));
    hle_ret_u64(c, 0);
}

/* Value constructors */
static void value_ctor_type(hle_call *c, uint32_t type) {
    BEGIN(c);
    guest_addr n = node_new(T_NULL);
    node_set_type(n, type);
    wr64(hle_arg_ptr(c, 0), n);
    END();
    hle_ret_u64(c, 0);
}
static void value_ctor(hle_call *c) { value_ctor_type(c, T_NULL); }
static void value_ctor_vtype(hle_call *c) { value_ctor_type(c, hle_arg_u32(c, 1) <= 7 ? hle_arg_u32(c, 1) : T_NULL); }

/* Value::set(...) and the matching constructors: `ctor` first gives the Value its node */
static void value_put(hle_call *c, bool ctor, int kind) {
    BEGIN(c);
    guest_addr v = hle_arg_ptr(c, 0), n;
    if (ctor) {
        n = node_new(T_NULL);
        wr64(v, n);
    } else {
        n = value_node(v);
    }
    uint64_t a1 = hle_arg(c, 1);
    switch (kind) {
    case T_BOOL: {
        node_set_type(n, T_BOOL);
        uint8_t b = (uint8_t)(a1 & 0xFF) != 0;
        gmem_write(g_mem, n + N_BOOL, &b, 1);
        break;
    }
    case T_INT: node_set_type(n, T_INT), wr64(n + N_INT, a1); break;
    case T_UINT: node_set_type(n, T_UINT), wr64(n + N_UINT, a1); break;
    case T_REAL: {
        double d = arg_double(c);
        uint64_t bits;
        memcpy(&bits, &d, 8);
        node_set_type(n, T_REAL);
        wr64(n + N_REAL, bits);
        break;
    }
    case T_STRING: { /* char const* */
        char *s = NULL;
        size_t cap = 256, len = 0;
        node_set_type(n, T_STRING);
        if (a1) {
            for (;;) { /* the guest string, any length */
                char *ns = (char *)realloc(s, cap);
                if (!ns) break;
                s = ns;
                if (!hle_read_str(c, a1, s, cap) || strlen(s) + 1 < cap) break;
                cap *= 2;
            }
            len = s ? strlen(s) : 0;
        }
        str_set(n + N_STRING, s ? s : "", len);
        free(s);
        break;
    }
    case 100: { /* String const& */
        uint64_t len = 0;
        char *s = str_get(a1, &len);
        node_set_type(n, T_STRING);
        if (s) str_set(n + N_STRING, s, len);
        free(s);
        break;
    }
    case T_ARRAY: node_set_type(n, T_ARRAY), array_copy(n + N_ARRAY, a1); break;
    case T_OBJECT: node_set_type(n, T_OBJECT), object_copy(n + N_OBJECT, a1); break;
    case 101: /* Value const& */
        if (a1 != v) node_assign(n, value_node(a1));
        break;
    case 102: node_set_type(n, (uint32_t)a1 <= 7 ? (uint32_t)a1 : T_NULL); break; /* ValueType */
    }
    END();
    hle_ret_u64(c, kind == 101 && !ctor ? v : 0); /* operator= returns *this */
}
#define PUT(name, ctor, kind)                                                                                          \
    static void name(hle_call *c) { value_put(c, ctor, kind); }
PUT(value_ctor_bool, true, T_BOOL)
PUT(value_ctor_long, true, T_INT)
PUT(value_ctor_ulong, true, T_UINT)
PUT(value_ctor_double, true, T_REAL)
PUT(value_ctor_cstr, true, T_STRING)
PUT(value_ctor_string, true, 100)
PUT(value_ctor_array, true, T_ARRAY)
PUT(value_ctor_object, true, T_OBJECT)
PUT(value_ctor_copy, true, 101)
PUT(value_set_bool, false, T_BOOL)
PUT(value_set_long, false, T_INT)
PUT(value_set_ulong, false, T_UINT)
PUT(value_set_double, false, T_REAL)
PUT(value_set_cstr, false, T_STRING)
PUT(value_set_string, false, 100)
PUT(value_set_array, false, T_ARRAY)
PUT(value_set_object, false, T_OBJECT)
PUT(value_set_value, false, 101)
PUT(value_set_type, false, 102)

static void value_dtor(hle_call *c) {
    BEGIN(c);
    guest_addr v = hle_arg_ptr(c, 0);
    node_free(rd64(v));
    wr64(v, 0);
    END();
    hle_ret_u64(c, 0);
}
static void value_clear(hle_call *c) {
    BEGIN(c);
    node_clear(value_node(hle_arg_ptr(c, 0)));
    END();
    hle_ret_u64(c, 0);
}

/* Primitive getters return const references in the guest C++ ABI, just like
 * strings/arrays/objects. Callers dereference RAX (including getReal); XMM0
 * and scalar values are not their return channels. Integer/unsigned getters
 * accept each other's representation, and real also accepts integers. */
static void value_get_type(hle_call *c) {
    BEGIN(c);
    uint32_t type = rd32(value_node(hle_arg_ptr(c, 0)) + N_TYPE);
    END();
    hle_ret_u32(c, type);
}
static void value_get_bool(hle_call *c) {
    BEGIN(c);
    guest_addr n = typed_node(c, hle_arg_ptr(c, 0), T_BOOL);
    END();
    hle_ret_u64(c, n + N_BOOL);
}
static void value_get_int(hle_call *c, bool unsign) {
    BEGIN(c);
    guest_addr n = value_node(hle_arg_ptr(c, 0));
    uint32_t type = rd32(n + N_TYPE);
    uint32_t field;
    if (type == T_INT) field = N_INT;
    else if (type == T_UINT) field = N_UINT;
    else {
        n = typed_node(c, hle_arg_ptr(c, 0), unsign ? T_UINT : T_INT);
        field = unsign ? N_UINT : N_INT;
    }
    END();
    hle_ret_u64(c, n + field);
}
static void value_get_integer(hle_call *c) { value_get_int(c, false); }
static void value_get_uinteger(hle_call *c) { value_get_int(c, true); }
static void value_get_real(hle_call *c) {
    BEGIN(c);
    guest_addr n = value_node(hle_arg_ptr(c, 0));
    uint32_t type = rd32(n + N_TYPE);
    if (type == T_INT || type == T_UINT) {
        double d = type == T_INT ? (double)(int64_t)rd64(n + N_INT) : (double)rd64(n + N_UINT);
        uint64_t bits;
        memcpy(&bits, &d, 8);
        wr64(n + N_REAL, bits); /* stable backing storage for the converted reference */
    } else {
        n = typed_node(c, hle_arg_ptr(c, 0), T_REAL);
    }
    END();
    hle_ret_u64(c, n + N_REAL);
}
static void value_get_ref(hle_call *c, uint32_t type, uint32_t field) {
    BEGIN(c);
    guest_addr n = typed_node(c, hle_arg_ptr(c, 0), type);
    END();
    hle_ret_u64(c, n + field);
}
static void value_get_string(hle_call *c) { value_get_ref(c, T_STRING, N_STRING); }
static void value_get_array(hle_call *c) { value_get_ref(c, T_ARRAY, N_ARRAY); }
static void value_get_object(hle_call *c) { value_get_ref(c, T_OBJECT, N_OBJECT); }

static void value_count(hle_call *c) {
    BEGIN(c);
    guest_addr n = value_node(hle_arg_ptr(c, 0));
    uint32_t type = rd32(n + N_TYPE);
    guest_addr list = type == T_ARRAY ? rd64(n + N_ARRAY) : type == T_OBJECT ? rd64(n + N_OBJECT) : 0;
    uint64_t count = rd64(list);
    END();
    hle_ret_u64(c, count);
}

/* The element `index` of an array Value, 0 if none */
static guest_addr element(guest_addr v, uint64_t index) {
    guest_addr n = value_node(v);
    if (rd32(n + N_TYPE) != T_ARRAY) return 0;
    guest_addr list = rd64(n + N_ARRAY), cell = list ? rd64(list + 8) : 0;
    while (cell && index--) cell = rd64(cell);
    return cell ? cell + 8 : 0;
}
/* The member `key` (host text) of an object Value, 0 if none */
static guest_addr member(guest_addr v, const char *key, uint64_t len) {
    guest_addr n = value_node(v);
    if (rd32(n + N_TYPE) != T_OBJECT) return 0;
    guest_addr pair = object_find(n + N_OBJECT, key, len);
    return pair ? pair + 16 : 0;
}

/* operator[](size_t) const / getValue(size_t) const: a default when absent; referValue(size_t): null */
static void value_index(hle_call *c, bool refer) {
    BEGIN(c);
    guest_addr v = hle_arg_ptr(c, 0), r = element(v, hle_arg(c, 1));
    if (!r && !refer) r = null_access(c, T_NULL, v);
    END();
    hle_ret_u64(c, r);
}
static void value_at(hle_call *c) { value_index(c, false); }
static void value_refer_index(hle_call *c) { value_index(c, true); }

/* operator[](char const*) const */
static void value_key_cstr(hle_call *c) {
    char key[1024];
    BEGIN(c);
    guest_addr v = hle_arg_ptr(c, 0), r = 0;
    if (hle_read_str(c, hle_arg_ptr(c, 1), key, sizeof(key))) r = member(v, key, strlen(key));
    if (!r) r = null_access(c, T_NULL, v);
    END();
    hle_ret_u64(c, r);
}
/* getValue(String const&) const; referValue(String const&): null when absent */
static void value_key_string(hle_call *c, bool refer) {
    BEGIN(c);
    guest_addr v = hle_arg_ptr(c, 0), r = 0;
    uint64_t len = 0;
    char *key = str_get(hle_arg_ptr(c, 1), &len);
    if (key) r = member(v, key, len);
    free(key);
    if (!r && !refer) r = null_access(c, T_NULL, v);
    END();
    hle_ret_u64(c, r);
}
static void value_get_value_string(hle_call *c) { value_key_string(c, false); }
static void value_refer_value_string(hle_call *c) { value_key_string(c, true); }

/* referArray / referObject: a null value becomes that container */
static void value_refer(hle_call *c, uint32_t type, uint32_t field) {
    BEGIN(c);
    guest_addr n = value_node(hle_arg_ptr(c, 0)), r = 0;
    if (rd32(n + N_TYPE) == T_NULL) node_set_type(n, type);
    if (rd32(n + N_TYPE) == type) r = n + field;
    else LOG_W(HLE, "sce::Json::Value::refer%s on a value of type %u", type == T_ARRAY ? "Array" : "Object", rd32(n + N_TYPE));
    END();
    hle_ret_u64(c, r);
}
static void value_refer_array(hle_call *c) { value_refer(c, T_ARRAY, N_ARRAY); }
static void value_refer_object(hle_call *c) { value_refer(c, T_OBJECT, N_OBJECT); }

/* serialize(String&) / toString(String&) const (a string's own text) */
static void value_text(hle_call *c, bool to_string) {
    BEGIN(c);
    guest_addr n = value_node(hle_arg_ptr(c, 0)), out = hle_arg_ptr(c, 1);
    if (to_string && rd32(n + N_TYPE) == T_STRING) {
        uint64_t len = 0;
        char *s = str_get(n + N_STRING, &len);
        if (s) str_set(out, s, len);
        free(s);
    } else {
        sbuf b = {0};
        serialize(&b, n, 0);
        str_set(out, b.p ? b.p : "", b.len);
        free(b.p);
    }
    END();
    hle_ret_s32(c, 0);
}
static void value_serialize(hle_call *c) { value_text(c, false); }
static void value_to_string(hle_call *c) { value_text(c, true); }

/* String */
static guest_addr g_empty_text; /* c_str of an empty String */
static void string_ctor(hle_call *c) {
    uint64_t zero = 0;
    gmem_write(c->mem, hle_arg_ptr(c, 0), &zero, 8);
    hle_ret_u64(c, 0);
}
static void string_ctor_cstr(hle_call *c) {
    char *s = NULL;
    size_t cap = 256;
    for (guest_addr a = hle_arg_ptr(c, 1); a;) {
        char *ns = (char *)realloc(s, cap);
        if (!ns) break;
        s = ns;
        if (!hle_read_str(c, a, s, cap) || strlen(s) + 1 < cap) break;
        cap *= 2;
    }
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0);
    wr64(self, 0);
    if (s) str_set(self, s, strlen(s));
    END();
    free(s);
    hle_ret_u64(c, 0);
}
static void string_copy(hle_call *c, bool ctor) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0), src = hle_arg_ptr(c, 1);
    if (ctor) wr64(self, 0);
    if (self != src) {
        uint64_t len = 0;
        char *s = str_get(src, &len);
        if (s) str_set(self, s, len);
        free(s);
    }
    END();
    hle_ret_u64(c, ctor ? 0 : self);
}
static void string_ctor_copy(hle_call *c) { string_copy(c, true); }
static void string_assign(hle_call *c) { string_copy(c, false); }
static void string_dtor(hle_call *c) {
    BEGIN(c);
    str_clear(hle_arg_ptr(c, 0));
    END();
    hle_ret_u64(c, 0);
}
static void string_c_str(hle_call *c) {
    BEGIN(c);
    guest_addr text = rd64(hle_arg_ptr(c, 0)), r;
    if (text) {
        r = text + 8;
    } else {
        if (!g_empty_text) g_empty_text = jalloc(16);
        r = g_empty_text;
    }
    END();
    hle_ret_u64(c, r);
}
static void string_length(hle_call *c) {
    BEGIN(c);
    uint64_t len = rd64(rd64(hle_arg_ptr(c, 0)));
    END();
    hle_ret_u64(c, len);
}
static void string_empty(hle_call *c) {
    BEGIN(c);
    uint64_t len = rd64(rd64(hle_arg_ptr(c, 0)));
    END();
    hle_ret_u32(c, len == 0);
}
static void string_compare(hle_call *c, bool eq) {
    char other[1024];
    if (!hle_read_str(c, hle_arg_ptr(c, 1), other, sizeof(other))) other[0] = 0;
    BEGIN(c);
    char *s = str_get(hle_arg_ptr(c, 0), NULL);
    int r = s ? strcmp(s, other) : 0;
    free(s);
    END();
    if (eq) hle_ret_u32(c, r == 0);
    else hle_ret_s32(c, r);
}
static void string_compare_cstr(hle_call *c) { string_compare(c, false); }
static void string_eq_cstr(hle_call *c) { string_compare(c, true); }

/* Array */
static void array_ctor(hle_call *c) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0);
    wr64(self, 0);
    list_of(self);
    END();
    hle_ret_u64(c, 0);
}
static void array_ctor_copy(hle_call *c) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0);
    wr64(self, 0);
    array_copy(self, hle_arg_ptr(c, 1));
    END();
    hle_ret_u64(c, 0);
}
static void array_dtor(hle_call *c) {
    BEGIN(c);
    array_clear(hle_arg_ptr(c, 0));
    END();
    hle_ret_u64(c, 0);
}
static void array_clear_(hle_call *c) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0);
    array_clear(self);
    list_of(self);
    END();
    hle_ret_u64(c, 0);
}
static void array_push_back(hle_call *c) {
    BEGIN(c);
    array_push_copy(hle_arg_ptr(c, 0), value_node(hle_arg_ptr(c, 1)));
    END();
    hle_ret_u64(c, 0);
}
static void container_size(hle_call *c) {
    BEGIN(c);
    uint64_t n = rd64(rd64(hle_arg_ptr(c, 0)));
    END();
    hle_ret_u64(c, n);
}
static void container_empty(hle_call *c) {
    BEGIN(c);
    uint64_t n = rd64(rd64(hle_arg_ptr(c, 0)));
    END();
    hle_ret_u32(c, n == 0);
}
/* begin() / end() return the iterator through the hidden result pointer (arg 0, `this` arg 1) */
static void container_begin(hle_call *c) {
    BEGIN(c);
    guest_addr out = hle_arg_ptr(c, 0), list = rd64(hle_arg_ptr(c, 1));
    wr64(out, list ? rd64(list + 8) : 0);
    END();
    hle_ret_u64(c, out);
}
static void container_end(hle_call *c) {
    BEGIN(c);
    guest_addr out = hle_arg_ptr(c, 0);
    wr64(out, 0);
    END();
    hle_ret_u64(c, out);
}
/* front() / back(): the first / last element's Value */
static void array_end_element(hle_call *c, bool back) {
    BEGIN(c);
    guest_addr list = rd64(hle_arg_ptr(c, 0)), cell = list ? rd64(list + (back ? 16 : 8)) : 0;
    guest_addr r = cell ? cell + 8 : null_access(c, T_NULL, 0);
    END();
    hle_ret_u64(c, r);
}
static void array_front(hle_call *c) { array_end_element(c, false); }
static void array_back(hle_call *c) { array_end_element(c, true); }

/* Iterators: u64 cell. operator* / -> give the Value (array) or the Pair (object) */
static void iter_next(hle_call *c) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0);
    wr64(self, rd64(rd64(self)));
    END();
    hle_ret_u64(c, self);
}
static void iter_ne(hle_call *c) {
    BEGIN(c);
    bool ne = rd64(hle_arg_ptr(c, 0)) != rd64(hle_arg_ptr(c, 1));
    END();
    hle_ret_u32(c, ne);
}
static void iter_deref(hle_call *c) {
    BEGIN(c);
    guest_addr cell = rd64(hle_arg_ptr(c, 0));
    END();
    hle_ret_u64(c, cell ? cell + 8 : 0);
}

/* Object */
static void object_ctor(hle_call *c) { array_ctor(c); }
static void object_ctor_copy(hle_call *c) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0);
    wr64(self, 0);
    object_copy(self, hle_arg_ptr(c, 1));
    END();
    hle_ret_u64(c, 0);
}
static void object_assign(hle_call *c) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0), src = hle_arg_ptr(c, 1);
    if (self != src) object_copy(self, src);
    END();
    hle_ret_u64(c, self);
}
static void object_dtor(hle_call *c) {
    BEGIN(c);
    object_clear(hle_arg_ptr(c, 0));
    END();
    hle_ret_u64(c, 0);
}
static void object_clear_(hle_call *c) {
    BEGIN(c);
    guest_addr self = hle_arg_ptr(c, 0);
    object_clear(self);
    list_of(self);
    END();
    hle_ret_u64(c, 0);
}
/* operator[](String const&): the member's Value, added (null) when absent */
static void object_subscript(hle_call *c) {
    BEGIN(c);
    uint64_t len = 0;
    char *key = str_get(hle_arg_ptr(c, 1), &len);
    guest_addr pair = key ? object_insert(hle_arg_ptr(c, 0), key, len) : 0;
    free(key);
    END();
    hle_ret_u64(c, pair ? pair + 16 : 0);
}
/* find(String const&) const: an iterator (hidden result pointer), the end when absent */
static void object_find_(hle_call *c) {
    BEGIN(c);
    guest_addr out = hle_arg_ptr(c, 0);
    uint64_t len = 0;
    char *key = str_get(hle_arg_ptr(c, 2), &len);
    guest_addr pair = key ? object_find(hle_arg_ptr(c, 1), key, len) : 0;
    free(key);
    wr64(out, pair ? pair - 8 : 0);
    END();
    hle_ret_u64(c, out);
}

/* Parser */
static void parser_parse_text(hle_call *c) {
    guest_addr text = hle_arg_ptr(c, 1);
    uint64_t size = hle_arg(c, 2);
    char *buf = size ? (char *)malloc(size) : NULL;
    if (size && (!buf || !gmem_read(c->mem, text, buf, size))) {
        free(buf);
        hle_ret_s32(c, JSON_ERROR_PARSE);
        return;
    }
    BEGIN(c);
    int32_t r = parse_into(hle_arg_ptr(c, 0), buf ? buf : "", size);
    END();
    free(buf);
    hle_ret_s32(c, r);
}
/* parse(Value&, int (*read)(char &, void *), void *ctx): characters until read fails or gives NUL */
static void parser_parse_callback(hle_call *c) {
    guest_addr fn = hle_arg_ptr(c, 1), ctx = hle_arg_ptr(c, 2);
    BEGIN(c);
    guest_addr ch = jalloc(16);
    END();
    sbuf b = {0};
    for (uint64_t rax = 0; fn && ch && b.len < (64u << 20);) {
        if (!runtime_call_nested(c->rt, fn, ch, ctx, 0, &rax) || (int32_t)rax != 0) break;
        char v = 0;
        gmem_read(c->mem, ch, &v, 1);
        if (!v) break;
        sb_put(&b, &v, 1);
    }
    BEGIN(c);
    jfree(ch);
    int32_t r = parse_into(hle_arg_ptr(c, 0), b.p ? b.p : "", b.len);
    END();
    free(b.p);
    hle_ret_s32(c, r);
}

#define J "libSceJson2"
#define F(name, fn) HLE_FUNC_ENTRY(J, name, fn)

static const hle_export k_exports[] = {
    F("_ZN3sce4Json11InitializerC1Ev", ret_zero),
    F("_ZN3sce4Json11InitializerC2Ev", ret_zero),
    F("_ZN3sce4Json11InitializerD1Ev", ret_zero),
    F("_ZN3sce4Json11InitializerD2Ev", ret_zero),
    F("_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE", ret_zero),
    F("_ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E", ret_zero),
    F("_ZN3sce4Json11Initializer9terminateEv", ret_zero),
    F("_ZN3sce4Json11Initializer27setGlobalNullAccessCallBackEPFRKNS0_5ValueENS0_9ValueTypeEPS3_PvES7_", initializer_set_null_callback),
    F("_ZN3sce4Json11Initializer27setGlobalNullAccessCallbackEPFRKNS0_5ValueENS0_9ValueTypeEPS3_PvES7_", initializer_set_null_callback),
    F("_ZN3sce4Json12MemAllocatorC2Ev", ret_zero),
    F("_ZN3sce4Json12MemAllocatorD2Ev", ret_zero),
    F("_ZN3sce4Json14InitParameter2C1Ev", init_param2_ctor),
    F("_ZN3sce4Json14InitParameter2C2Ev", init_param2_ctor),
    F("_ZN3sce4Json14InitParameter212setAllocatorEPNS0_12MemAllocatorEPv", init_param2_set_allocator),
    F("_ZN3sce4Json14InitParameter217setFileBufferSizeEm", init_param2_set_file_buffer_size),
    F("_ZN3sce4Json14InitParameter225setSpecialFloatFormatTypeENS0_22SpecialFloatFormatTypeE", init_param2_set_float_format),
    F("_ZN3sce4Json5Value21setNullAccessCallBackEPFRKS1_NS0_9ValueTypeEPS2_PvES6_", value_set_null_callback),
    F("_ZN3sce4Json5Value22setSpecialFloatHandlerEPFKS1_NS0_12FunctionTypeEdPS2_PvPbES5_", value_set_special_float_handler),
    /* wrong types and missing elements go to the null access callback, as described at the top */
    F("_ZN3sce4Json5Value22setTypeMismatchHandlerEPFRKS1_NS0_9ValueTypeEPS2_S5_PvES6_", value_set_special_float_handler),
    F("_ZN3sce4Json5Value30setElementAccessFailureHandlerEPFviPKNS0_6StringEPKmPKS1_S8_PvES9_", value_set_special_float_handler),
    /* MemAllocator::notifyError(int, size_t, void *): the base class does nothing */
    F("_ZN3sce4Json12MemAllocator11notifyErrorEimPv", value_set_special_float_handler),
    F("_ZN3sce4Json17InitParameterRttiC1EPNS0_14AllocParamRttiEPvm", init_param_rtti_ctor),
    F("_ZN3sce4Json18InitParameterRtti216setAllocatorRttiEPNS0_14AllocParamRttiEPv", init_param2_set_allocator),
    /* Value */
    F("_ZN3sce4Json5ValueC1Ev", value_ctor),
    F("_ZN3sce4Json5ValueC2Ev", value_ctor),
    F("_ZN3sce4Json5ValueC1ENS0_9ValueTypeE", value_ctor_vtype),
    F("_ZN3sce4Json5ValueC1Eb", value_ctor_bool),
    F("_ZN3sce4Json5ValueC1El", value_ctor_long),
    F("_ZN3sce4Json5ValueC1Em", value_ctor_ulong),
    F("_ZN3sce4Json5ValueC1Ed", value_ctor_double),
    F("_ZN3sce4Json5ValueC1EPKc", value_ctor_cstr),
    F("_ZN3sce4Json5ValueC1ERKNS0_6StringE", value_ctor_string),
    F("_ZN3sce4Json5ValueC1ERKNS0_5ArrayE", value_ctor_array),
    F("_ZN3sce4Json5ValueC1ERKNS0_6ObjectE", value_ctor_object),
    F("_ZN3sce4Json5ValueC1ERKS1_", value_ctor_copy),
    F("_ZN3sce4Json5ValueD1Ev", value_dtor),
    F("_ZN3sce4Json5ValueD2Ev", value_dtor),
    F("_ZN3sce4Json5ValueaSERKS1_", value_set_value),
    F("_ZN3sce4Json5Value3setEb", value_set_bool),
    F("_ZN3sce4Json5Value3setEl", value_set_long),
    F("_ZN3sce4Json5Value3setEm", value_set_ulong),
    F("_ZN3sce4Json5Value3setEd", value_set_double),
    F("_ZN3sce4Json5Value3setEPKc", value_set_cstr),
    F("_ZN3sce4Json5Value3setERKNS0_6StringE", value_set_string),
    F("_ZN3sce4Json5Value3setERKNS0_5ArrayE", value_set_array),
    F("_ZN3sce4Json5Value3setERKNS0_6ObjectE", value_set_object),
    F("_ZN3sce4Json5Value3setERKS1_", value_set_value),
    F("_ZN3sce4Json5Value3setENS0_9ValueTypeE", value_set_type),
    F("_ZN3sce4Json5Value5clearEv", value_clear),
    F("_ZNK3sce4Json5Value7getTypeEv", value_get_type),
    F("_ZNK3sce4Json5Value10getBooleanEv", value_get_bool),
    F("_ZNK3sce4Json5Value10getIntegerEv", value_get_integer),
    F("_ZNK3sce4Json5Value11getUIntegerEv", value_get_uinteger),
    F("_ZNK3sce4Json5Value7getRealEv", value_get_real),
    F("_ZNK3sce4Json5Value9getStringEv", value_get_string),
    F("_ZNK3sce4Json5Value8getArrayEv", value_get_array),
    F("_ZNK3sce4Json5Value9getObjectEv", value_get_object),
    F("_ZNK3sce4Json5Value5countEv", value_count),
    F("_ZNK3sce4Json5ValueixEm", value_at),
    F("_ZNK3sce4Json5Value8getValueEm", value_at),
    F("_ZNK3sce4Json5ValueixEPKc", value_key_cstr),
    F("_ZNK3sce4Json5Value8getValueERKNS0_6StringE", value_get_value_string),
    F("_ZN3sce4Json5Value10referValueEm", value_refer_index),
    F("_ZN3sce4Json5Value10referValueERKNS0_6StringE", value_refer_value_string),
    F("_ZN3sce4Json5Value10referArrayEv", value_refer_array),
    F("_ZN3sce4Json5Value11referObjectEv", value_refer_object),
    F("_ZN3sce4Json5Value9serializeERNS0_6StringE", value_serialize),
    F("_ZNK3sce4Json5Value8toStringERNS0_6StringE", value_to_string),
    /* String */
    F("_ZN3sce4Json6StringC1Ev", string_ctor),
    F("_ZN3sce4Json6StringC2Ev", string_ctor),
    F("_ZN3sce4Json6StringC1EPKc", string_ctor_cstr),
    F("_ZN3sce4Json6StringC2EPKc", string_ctor_cstr),
    F("_ZN3sce4Json6StringC1ERKS1_", string_ctor_copy),
    F("_ZN3sce4Json6StringD1Ev", string_dtor),
    F("_ZN3sce4Json6StringD2Ev", string_dtor),
    F("_ZN3sce4Json6StringaSERKS1_", string_assign),
    F("_ZNK3sce4Json6String5c_strEv", string_c_str),
    F("_ZNK3sce4Json6String6lengthEv", string_length),
    F("_ZNK3sce4Json6String4sizeEv", string_length),
    F("_ZNK3sce4Json6String5emptyEv", string_empty),
    F("_ZNK3sce4Json6String7compareEPKc", string_compare_cstr),
    F("_ZNK3sce4Json6StringeqEPKc", string_eq_cstr),
    /* Array */
    F("_ZN3sce4Json5ArrayC1Ev", array_ctor),
    F("_ZN3sce4Json5ArrayC2Ev", array_ctor),
    F("_ZN3sce4Json5ArrayC1ERKS1_", array_ctor_copy),
    F("_ZN3sce4Json5ArrayD1Ev", array_dtor),
    F("_ZN3sce4Json5ArrayD2Ev", array_dtor),
    F("_ZN3sce4Json5Array5clearEv", array_clear_),
    F("_ZN3sce4Json5Array9push_backERKNS0_5ValueE", array_push_back),
    F("_ZNK3sce4Json5Array4sizeEv", container_size),
    F("_ZNK3sce4Json5Array5emptyEv", container_empty),
    F("_ZNK3sce4Json5Array5beginEv", container_begin),
    F("_ZNK3sce4Json5Array3endEv", container_end),
    F("_ZNK3sce4Json5Array5frontEv", array_front),
    F("_ZNK3sce4Json5Array4backEv", array_back),
    F("_ZN3sce4Json5Array8iteratorD1Ev", ret_zero),
    F("_ZN3sce4Json5Array8iteratorD2Ev", ret_zero),
    F("_ZN3sce4Json5Array8iteratorppEv", iter_next),
    F("_ZNK3sce4Json5Array8iteratorneERKS2_", iter_ne),
    F("_ZNK3sce4Json5Array8iteratordeEv", iter_deref),
    F("_ZNK3sce4Json5Array8iteratorptEv", iter_deref),
    /* Object */
    F("_ZN3sce4Json6ObjectC1Ev", object_ctor),
    F("_ZN3sce4Json6ObjectC2Ev", object_ctor),
    F("_ZN3sce4Json6ObjectC1ERKS1_", object_ctor_copy),
    F("_ZN3sce4Json6ObjectD1Ev", object_dtor),
    F("_ZN3sce4Json6ObjectD2Ev", object_dtor),
    F("_ZN3sce4Json6ObjectaSERKS1_", object_assign),
    F("_ZN3sce4Json6Object5clearEv", object_clear_),
    F("_ZN3sce4Json6ObjectixERKNS0_6StringE", object_subscript),
    F("_ZNK3sce4Json6Object4findERKNS0_6StringE", object_find_),
    F("_ZNK3sce4Json6Object4sizeEv", container_size),
    F("_ZNK3sce4Json6Object5emptyEv", container_empty),
    F("_ZNK3sce4Json6Object5beginEv", container_begin),
    F("_ZNK3sce4Json6Object3endEv", container_end),
    F("_ZN3sce4Json6Object8iteratorD1Ev", ret_zero),
    F("_ZN3sce4Json6Object8iteratorD2Ev", ret_zero),
    F("_ZN3sce4Json6Object8iteratorppEv", iter_next),
    F("_ZNK3sce4Json6Object8iteratorneERKS2_", iter_ne),
    F("_ZNK3sce4Json6Object8iteratordeEv", iter_deref),
    F("_ZNK3sce4Json6Object8iteratorptEv", iter_deref),
    /* Parser */
    F("_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", parser_parse_text),
    F("_ZN3sce4Json6Parser5parseERNS0_5ValueEPFiRcPvES5_", parser_parse_callback),
};

void hle_register_json2(void) { hle_register_table(k_exports, PS5C_ARRAY_COUNT(k_exports)); }
