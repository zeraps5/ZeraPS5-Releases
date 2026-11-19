/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu/shader/spv.h"

#include <stdlib.h>
#include <string.h>

#define SPV_MAGIC 0x07230203u
#define SPV_VERSION_1_3 0x00010300u

static void push(spv_builder *b, spv_words *w, uint32_t v) {
    if (w->len == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 256;
        uint32_t *d = (uint32_t *)realloc(w->data, cap * sizeof(uint32_t));
        if (!d) {
            b->failed = true;
            return;
        }
        w->data = d;
        w->cap = cap;
    }
    w->data[w->len++] = v;
}

/* Keep declarations separate from initialization: helpers may discover a
 * local after emitting prologue instructions. All locals must lead block 1. */
static spv_words *body(spv_builder *b) { return &b->sec[SPV_SEC_FUNCTION]; }

void spv_init(spv_builder *b) {
    memset(b, 0, sizeof(*b));
    b->next_id = 1;
}

void spv_free(spv_builder *b) {
    for (int i = 0; i < SPV_SEC_COUNT; i++) free(b->sec[i].data);
    free(b->fn_vars.data);
    free(b->fn_prologue.data);
    free(b->interned);
    free(b->intern_slots);
    memset(b, 0, sizeof(*b));
}

uint32_t spv_id(spv_builder *b) { return b->next_id++; }

static void emit_to(spv_builder *b, spv_words *w, uint32_t op, const uint32_t *operands, uint32_t n) {
    push(b, w, ((n + 1) << 16) | op);
    for (uint32_t i = 0; i < n; i++) push(b, w, operands[i]);
}

void spv_emit(spv_builder *b, spv_section s, uint32_t op, const uint32_t *operands, uint32_t n) {
    emit_to(b, &b->sec[s], op, operands, n);
}

uint32_t spv_string(const char *str, uint32_t *out, uint32_t max_words) {
    size_t len = strlen(str) + 1;
    uint32_t words = (uint32_t)((len + 3) / 4);
    if (words > max_words) return 0;
    memset(out, 0, words * 4);
    memcpy(out, str, len);
    return words;
}

bool spv_finish(spv_builder *b, uint32_t **out, size_t *words) {
    if (b->failed) return false;
    size_t total = 5;
    for (int i = 0; i < SPV_SEC_COUNT; i++) total += b->sec[i].len;
    uint32_t *m = (uint32_t *)malloc(total * sizeof(uint32_t));
    if (!m) return false;
    m[0] = SPV_MAGIC;
    m[1] = SPV_VERSION_1_3;
    m[2] = 0x50533543u; /* generator: "PS5C" */
    m[3] = b->next_id;
    m[4] = 0;
    size_t pos = 5;
    for (int i = 0; i < SPV_SEC_COUNT; i++) {
        memcpy(m + pos, b->sec[i].data, b->sec[i].len * sizeof(uint32_t));
        pos += b->sec[i].len;
    }
    *out = m;
    *words = total;
    return true;
}

/* ---- Interning ---- */

static uint32_t intern_hash(const uint32_t *key, uint32_t len) {
    uint32_t h = 2166136261u ^ len;
    for (uint32_t i = 0; i < len; i++) h = (h ^ key[i]) * 16777619u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    return h;
}

static bool intern_grow_index(spv_builder *b) {
    size_t cap = b->intern_slot_cap ? b->intern_slot_cap * 2 : 128;
    if (cap < b->intern_slot_cap || cap > SIZE_MAX / sizeof(uint32_t) || b->intern_len >= UINT32_MAX) {
        b->failed = true;
        return false;
    }
    uint32_t *slots = calloc(cap, sizeof(*slots));
    if (!slots) {
        b->failed = true;
        return false;
    }
    for (size_t i = 0; i < b->intern_len; i++) {
        const spv_intern *e = b->interned + i;
        size_t at = intern_hash(e->key, e->len) & (cap - 1);
        while (slots[at]) at = (at + 1) & (cap - 1);
        slots[at] = (uint32_t)i + 1;
    }
    free(b->intern_slots);
    b->intern_slots = slots;
    b->intern_slot_cap = cap;
    return true;
}

static uint32_t intern(spv_builder *b, uint32_t op, const uint32_t *operands, uint32_t n, bool result_first) {
    /* key: opcode followed by the operands (excluding the result id) */
    uint32_t key[8] = {op};
    if (n > 7 || b->failed) return 0;
    if (n) memcpy(key + 1, operands, n * sizeof(uint32_t));
    if (!b->intern_slot_cap && !intern_grow_index(b)) return 0;
    uint32_t hash = intern_hash(key, n + 1);
    size_t at = hash & (b->intern_slot_cap - 1);
    while (b->intern_slots[at]) {
        const spv_intern *e = b->interned + b->intern_slots[at] - 1;
        if (e->len == n + 1 && !memcmp(e->key, key, (n + 1) * sizeof(uint32_t))) return e->id;
        at = (at + 1) & (b->intern_slot_cap - 1);
    }
    if (b->intern_len >= b->intern_slot_cap / 2) {
        if (!intern_grow_index(b)) return 0;
        at = hash & (b->intern_slot_cap - 1);
        while (b->intern_slots[at]) at = (at + 1) & (b->intern_slot_cap - 1);
    }
    if (b->intern_len == b->intern_cap) {
        size_t cap = b->intern_cap ? b->intern_cap * 2 : 64;
        if (cap < b->intern_cap || cap > SIZE_MAX / sizeof(spv_intern)) {
            b->failed = true;
            return 0;
        }
        spv_intern *d = realloc(b->interned, cap * sizeof(*d));
        if (!d) {
            b->failed = true;
            return 0;
        }
        b->interned = d;
        b->intern_cap = cap;
    }
    uint32_t id = spv_id(b);
    uint32_t words[8];
    if (result_first) {
        words[0] = id;
        if (n) memcpy(words + 1, operands, n * sizeof(uint32_t));
    } else { /* constants: result type first, then result id */
        words[0] = operands[0];
        words[1] = id;
        memcpy(words + 2, operands + 1, (n - 1) * sizeof(uint32_t));
    }
    spv_emit(b, SPV_SEC_GLOBAL, op, words, n + 1);
    if (b->failed) return 0;
    spv_intern *e = &b->interned[b->intern_len];
    memcpy(e->key, key, sizeof(key));
    e->len = n + 1;
    e->id = id;
    b->intern_slots[at] = (uint32_t)b->intern_len + 1;
    b->intern_len++;
    return id;
}

uint32_t spv_type_void(spv_builder *b) { return intern(b, SpvOpTypeVoid, NULL, 0, true); }
uint32_t spv_type_bool(spv_builder *b) { return intern(b, SpvOpTypeBool, NULL, 0, true); }

uint32_t spv_type_int(spv_builder *b, uint32_t width, bool is_signed) {
    uint32_t a[2] = {width, is_signed ? 1u : 0u};
    return intern(b, SpvOpTypeInt, a, 2, true);
}

uint32_t spv_type_float(spv_builder *b, uint32_t width) { return intern(b, SpvOpTypeFloat, &width, 1, true); }

uint32_t spv_type_vector(spv_builder *b, uint32_t component, uint32_t count) {
    uint32_t a[2] = {component, count};
    return intern(b, SpvOpTypeVector, a, 2, true);
}

uint32_t spv_type_array(spv_builder *b, uint32_t element, uint32_t length_id) {
    uint32_t a[2] = {element, length_id};
    return intern(b, SpvOpTypeArray, a, 2, true);
}

uint32_t spv_type_runtime_array(spv_builder *b, uint32_t element) {
    uint32_t id = spv_id(b);
    uint32_t a[2] = {id, element};
    spv_emit(b, SPV_SEC_GLOBAL, SpvOpTypeRuntimeArray, a, 2);
    return id;
}

uint32_t spv_type_pointer(spv_builder *b, uint32_t storage_class, uint32_t pointee) {
    uint32_t a[2] = {storage_class, pointee};
    return intern(b, SpvOpTypePointer, a, 2, true);
}

uint32_t spv_type_function(spv_builder *b, uint32_t ret, const uint32_t *params, uint32_t n) {
    uint32_t a[5] = {ret};
    if (n > 4) return 0;
    memcpy(a + 1, params, n * sizeof(uint32_t));
    return intern(b, SpvOpTypeFunction, a, n + 1, true);
}

uint32_t spv_type_image(spv_builder *b, const uint32_t operands[7]) { return intern(b, SpvOpTypeImage, operands, 7, true); }

uint32_t spv_type_sampled_image(spv_builder *b, uint32_t image) { return intern(b, SpvOpTypeSampledImage, &image, 1, true); }

uint32_t spv_const_u32(spv_builder *b, uint32_t v) {
    uint32_t a[2] = {spv_type_int(b, 32, false), v};
    return intern(b, SpvOpConstant, a, 2, false);
}

uint32_t spv_const_i32(spv_builder *b, int32_t v) {
    uint32_t a[2] = {spv_type_int(b, 32, true), (uint32_t)v};
    return intern(b, SpvOpConstant, a, 2, false);
}

uint32_t spv_const_f32(spv_builder *b, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    uint32_t a[2] = {spv_type_float(b, 32), bits};
    return intern(b, SpvOpConstant, a, 2, false);
}

uint32_t spv_const_bool(spv_builder *b, bool v) {
    uint32_t t = spv_type_bool(b);
    return intern(b, v ? SpvOpConstantTrue : SpvOpConstantFalse, &t, 1, false);
}

static const spv_intern *intern_by_id(const spv_builder *b, uint32_t id) {
    size_t lo = 0, hi = b->intern_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (b->interned[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < b->intern_len && b->interned[lo].id == id ? b->interned + lo : NULL;
}

bool spv_const_u32_value(const spv_builder *b, uint32_t id, uint32_t *value) {
    const spv_intern *e = intern_by_id(b, id);
    if (!value || !e || e->len != 3 || e->key[0] != SpvOpConstant) return false;
    const spv_intern *type = intern_by_id(b, e->key[1]);
    if (!type || type->len != 3 || type->key[0] != SpvOpTypeInt || type->key[1] != 32 || type->key[2]) return false;
    *value = e->key[2];
    return true;
}

uint32_t spv_const_composite(spv_builder *b, uint32_t type, const uint32_t *parts, uint32_t n) {
    uint32_t a[5] = {type};
    if (n > 4) return 0;
    memcpy(a + 1, parts, n * sizeof(uint32_t));
    return intern(b, SpvOpConstantComposite, a, n + 1, false);
}

/* ---- Function bodies ---- */

uint32_t spv_op(spv_builder *b, uint32_t op, uint32_t type, const uint32_t *args, uint32_t n) {
    uint32_t id = spv_id(b);
    spv_words *w = body(b);
    push(b, w, ((n + 3) << 16) | op);
    push(b, w, type);
    push(b, w, id);
    for (uint32_t i = 0; i < n; i++) push(b, w, args[i]);
    if (b->exact && (op == SpvOpFAdd || op == SpvOpFSub || op == SpvOpFMul || op == SpvOpFDiv || op == SpvOpDot))
        spv_decorate(b, id, SpvDecorationNoContraction, NULL, 0);
    return id;
}

void spv_op_void(spv_builder *b, uint32_t op, const uint32_t *args, uint32_t n) { emit_to(b, body(b), op, args, n); }

uint32_t spv_ext(spv_builder *b, uint32_t type, uint32_t glsl_op, const uint32_t *args, uint32_t n) {
    if (!b->glsl_ext) {
        b->glsl_ext = spv_id(b);
        uint32_t w[5] = {b->glsl_ext};
        uint32_t sw = spv_string("GLSL.std.450", w + 1, 4);
        spv_emit(b, SPV_SEC_IMPORT, SpvOpExtInstImport, w, 1 + sw);
    }
    uint32_t a[8] = {b->glsl_ext, glsl_op};
    if (n > 6) return 0;
    memcpy(a + 2, args, n * sizeof(uint32_t));
    uint32_t id=spv_op(b, SpvOpExtInst, type, a, n + 2);
    /* GLSL.std.450 permits Fma to change precision across modules unless
     * NoContraction is present. The exact mode must preserve a single
     * rounding for explicit guest FMA as well as separate MAD operations. */
    if(b->exact && glsl_op==GLSLstd450Fma)
        spv_decorate(b,id,SpvDecorationNoContraction,NULL,0);
    return id;
}

void spv_label(spv_builder *b, uint32_t id) { spv_op_void(b, SpvOpLabel, &id, 1); }

uint32_t spv_local_var(spv_builder *b, uint32_t pointer_type) {
    uint32_t id = spv_id(b);
    uint32_t a[3] = {pointer_type, id, SpvStorageClassFunction};
    emit_to(b, &b->fn_vars, SpvOpVariable, a, 3);
    return id;
}

uint32_t spv_prologue_op(spv_builder *b, uint32_t op, uint32_t type, const uint32_t *args, uint32_t n) {
    uint32_t id = spv_id(b);
    push(b, &b->fn_prologue, ((n + 3) << 16) | op);
    push(b, &b->fn_prologue, type);
    push(b, &b->fn_prologue, id);
    for (uint32_t i = 0; i < n; i++) push(b, &b->fn_prologue, args[i]);
    return id;
}

void spv_prologue_void(spv_builder *b, uint32_t op, const uint32_t *args, uint32_t n) {
    emit_to(b, &b->fn_prologue, op, args, n);
}

void spv_begin_function(spv_builder *b, uint32_t ret, uint32_t fn_type, uint32_t id) {
    uint32_t a[4] = {ret, id, SpvFunctionControlNone, fn_type};
    spv_op_void(b, SpvOpFunction, a, 4);
    b->function_vars_at = (uint32_t)body(b)->len;
    b->fn_vars.len = 0;
    b->fn_prologue.len = 0;
}

void spv_end_function(spv_builder *b) {
    spv_op_void(b, SpvOpFunctionEnd, NULL, 0);
    /* splice the variables right after the first OpLabel (2 words) */
    spv_words *w = body(b);
    size_t at = b->function_vars_at + 2, vars = b->fn_vars.len;
    if (b->fn_prologue.len > SIZE_MAX - vars) { b->failed = true; return; }
    size_t n = vars + b->fn_prologue.len;
    if (!n || at > w->len) return;
    for (size_t i = 0; i < n; i++) push(b, w, 0);
    if (b->failed) return;
    memmove(w->data + at + n, w->data + at, (w->len - n - at) * sizeof(uint32_t));
    if (vars) memcpy(w->data + at, b->fn_vars.data, vars * sizeof(uint32_t));
    if (b->fn_prologue.len) memcpy(w->data + at + vars, b->fn_prologue.data, b->fn_prologue.len * sizeof(uint32_t));
}

/* ---- Decorations ---- */

void spv_decorate(spv_builder *b, uint32_t target, uint32_t decoration, const uint32_t *args, uint32_t n) {
    uint32_t a[6] = {target, decoration};
    if (n > 4) return;
    memcpy(a + 2, args, n * sizeof(uint32_t));
    spv_emit(b, SPV_SEC_ANNOTATION, SpvOpDecorate, a, n + 2);
}

void spv_member_decorate(spv_builder *b, uint32_t type, uint32_t member, uint32_t decoration, const uint32_t *args,
                         uint32_t n) {
    uint32_t a[7] = {type, member, decoration};
    if (n > 4) return;
    memcpy(a + 3, args, n * sizeof(uint32_t));
    spv_emit(b, SPV_SEC_ANNOTATION, SpvOpMemberDecorate, a, n + 3);
}
