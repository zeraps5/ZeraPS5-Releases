"""Exercise the production snapshot policy with fake Vulkan allocations.

Does not test Vulkan submission/coherence, shader output or commercial games.
Usage: python tools/test_readonly_cache.py [C-compiler]
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/gpu/vk/vk_pipeline.c').read_text(encoding='utf-8')
entry = re.search(r'typedef struct readonly_entry \{.*?\} readonly_entry;', source, re.S)[0]
macros = '\n'.join(re.findall(r'^#define (?:MAX_READONLY_CACHE|READONLY_CACHE_\w+) .*$', source, re.M))
begin = source.index('static VkBuffer readonly_snapshot(')
brace = source.index('{', begin)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
function = source[begin:end]
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t guest_addr, VkDeviceSize, VkBuffer;
#define VK_NULL_HANDLE 0
#define VK_BUFFER_USAGE_STORAGE_BUFFER_BIT 1
typedef struct vk_buffer { VkBuffer buffer; VkDeviceSize size; void *mapped; } vk_buffer;
'''
support = r'''
typedef struct vk_pipelines {
    readonly_entry readonly[MAX_READONLY_CACHE];
    uint64_t readonly_bytes, readonly_sequence, batch;
    unsigned wb_count;
} vk_pipelines;
typedef struct renderer { vk_pipelines *pipes; void *mem; } renderer;
static uint8_t *guest;
static uint64_t next_handle = 1, allocations;
static bool fail_allocation, missing;
static int g_stats_on;
static uint64_t g_stat_ro_hits, g_stat_ro_misses, g_stat_ro_compared, g_stat_ro_compare_ns, g_stat_ro_fallback, g_stat_bytes;
static uint64_t stat_now(void) { return 0; }
static uint64_t pal_time_ns(void) { return 0; }
static const void *gmem_host(void *mem, guest_addr addr, uint64_t size) {
    (void)mem; (void)addr; (void)size; return missing ? NULL : guest;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static bool vk_buffer_create(renderer *r, VkDeviceSize size, unsigned usage, bool host, vk_buffer *b) {
    (void)r; (void)usage; (void)host;
    memset(b, 0, sizeof(*b));
    if (fail_allocation) return false;
    b->mapped = malloc((size_t)size);
    if (!b->mapped) return false;
    b->size = size; b->buffer = next_handle++; allocations++; return true;
}
static void vk_buffer_destroy(renderer *r, vk_buffer *b) {
    for (unsigned i = 0; i < MAX_READONLY_CACHE; i++) {
        readonly_entry *e = &r->pipes->readonly[i];
        if (b->buffer && e->buffer.buffer == b->buffer) CHECK(e->used_batch != r->pipes->batch);
    }
    free(b->mapped); memset(b, 0, sizeof(*b));
}
static readonly_entry *lookup(vk_pipelines *p, VkBuffer handle) {
    for (unsigned i = 0; i < MAX_READONLY_CACHE; i++) if (p->readonly[i].buffer.buffer == handle) return &p->readonly[i];
    return NULL;
}
static void clear(renderer *r) {
    r->pipes->batch++;
    for (unsigned i = 0; i < MAX_READONLY_CACHE; i++) vk_buffer_destroy(r, &r->pipes->readonly[i].buffer);
    memset(r->pipes, 0, sizeof(*r->pipes)); r->pipes->batch = 1;
}
'''
tests = r'''
int main(void) {
    vk_pipelines *p = calloc(1, sizeof(*p)); CHECK(p);
    renderer r = {.pipes = p}; p->batch = 1;
    guest = calloc(1, READONLY_CACHE_MAX); CHECK(guest);
    memset(guest, 0x5a, 4096);
    VkBuffer first = readonly_snapshot(&r, 0x1000, 4096); CHECK(first);
    CHECK(readonly_snapshot(&r, 0x1000, 4096) == first);
    CHECK(allocations == 1);
    CHECK(((uint8_t *)lookup(p, first)->buffer.mapped)[4096] == 0);
    CHECK(((uint8_t *)lookup(p, first)->buffer.mapped)[4111] == 0);
    /* A change away from a small sample must be detected; the old draw stays intact. */
    guest[3001] ^= 1;
    VkBuffer second = readonly_snapshot(&r, 0x1000, 4096); CHECK(second && second != first);
    CHECK(((uint8_t *)lookup(p, first)->buffer.mapped)[3001] == 0x5a);
    CHECK(((uint8_t *)lookup(p, second)->buffer.mapped)[3001] == guest[3001]);
    CHECK(readonly_snapshot(&r, 0x1000, 4096) == second);
    p->batch++;
    guest[3001] ^= 2;
    uint64_t before = allocations;
    CHECK(readonly_snapshot(&r, 0x1000, 4096));
    CHECK(allocations == before); /* completed version can be updated in place */
    p->wb_count = 1;
    CHECK(!readonly_snapshot(&r, 0x1000, 4096));
    p->wb_count = 0; missing = true;
    CHECK(!readonly_snapshot(&r, 0x1000, 4096)); missing = false;
    CHECK(!readonly_snapshot(&r, 0x1000, READONLY_CACHE_MIN - 1));
    CHECK(!readonly_snapshot(&r, 0x1000, READONLY_CACHE_MAX + 1));
    clear(&r);
    for (unsigned i = 0; i < MAX_READONLY_CACHE; i++) CHECK(readonly_snapshot(&r, 0x1000 + i, 4096));
    CHECK(!readonly_snapshot(&r, 0x999999, 4096)); /* no current-batch eviction */
    p->batch++;
    CHECK(readonly_snapshot(&r, 0x999999, 4096)); /* eviction after completion */
    clear(&r);
    uint64_t bucket = (READONLY_CACHE_MAX + 16 + 65535) & ~65535ull;
    unsigned count = (unsigned)(READONLY_CACHE_BUDGET / bucket);
    if (count > MAX_READONLY_CACHE) count = MAX_READONLY_CACHE;
    for (unsigned i = 0; i < count; i++) CHECK(readonly_snapshot(&r, 0x1000 + i, READONLY_CACHE_MAX));
    CHECK(!readonly_snapshot(&r, 0x999999, READONLY_CACHE_MAX));
    CHECK(p->readonly_bytes <= READONLY_CACHE_BUDGET);
    p->batch++;
    CHECK(readonly_snapshot(&r, 0x999999, READONLY_CACHE_MAX));
    CHECK(p->readonly_bytes <= READONLY_CACHE_BUDGET);
    clear(&r);
    fail_allocation = true;
    CHECK(!readonly_snapshot(&r, 0x1000, 4096));
    CHECK(p->readonly_bytes == 0);
    fail_allocation = false;
    CHECK(readonly_snapshot(&r, 0x1000, 4096));
    clear(&r); free(guest); free(p);
    puts("PASS: exact data comparison, immutable in-flight versions, fence epochs, bounds, holes, writebacks, capacity, budget, allocation failure");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='zeraps5-readonly-policy-') as temp:
    folder = Path(temp)
    file = folder / 'test.c'
    file.write_text(prefix + macros + '\n' + entry + support + function + tests, encoding='utf-8')
    exe = folder / 'test.exe'
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'cc', '-std=c11', '-O2', '-Wall', '-Wextra', str(file), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
