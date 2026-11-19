/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "hle/guest_printf.h"

#include <stdio.h>
#include <string.h>

#define VA_GP_LIMIT 48u  /* 6 registers * 8 */
#define VA_FP_LIMIT 176u /* 48 + 8 registers * 16 */

void guest_va_from_call(guest_va *va, hle_call *c, uint32_t fixed_gp) {
    memset(va, 0, sizeof(*va));
    va->c = c;
    va->gp = fixed_gp;
    va->overflow = c->a->stack;
}

void guest_va_from_list(guest_va *va, hle_call *c, guest_addr va_list) {
    memset(va, 0, sizeof(*va));
    va->c = c;
    va->va_list = va_list;
}

static uint64_t read_u64(hle_call *c, guest_addr addr) {
    uint64_t v = 0;
    gmem_read_u64(c->mem, addr, &v);
    return v;
}

uint64_t guest_va_int(guest_va *va) {
    if (!va->va_list) {
        if (va->gp < 6) return va->c->a->gpr[va->gp++];
        uint64_t v = read_u64(va->c, va->overflow);
        va->overflow += 8;
        return v;
    }
    uint32_t gp_offset = 0;
    gmem_read(va->c->mem, va->va_list, &gp_offset, 4);
    if (gp_offset < VA_GP_LIMIT) {
        uint64_t v = read_u64(va->c, read_u64(va->c, va->va_list + 16) + gp_offset);
        gp_offset += 8;
        gmem_write(va->c->mem, va->va_list, &gp_offset, 4);
        return v;
    }
    guest_addr area = read_u64(va->c, va->va_list + 8);
    uint64_t v = read_u64(va->c, area);
    gmem_write_u64(va->c->mem, va->va_list + 8, area + 8);
    return v;
}

double guest_va_double(guest_va *va) {
    uint64_t bits;
    if (!va->va_list) {
        if (va->fp < 8) {
            bits = va->c->a->xmm[va->fp++][0];
        } else {
            bits = read_u64(va->c, va->overflow);
            va->overflow += 8;
        }
    } else {
        uint32_t fp_offset = 0;
        gmem_read(va->c->mem, va->va_list + 4, &fp_offset, 4);
        if (fp_offset < VA_FP_LIMIT) {
            bits = read_u64(va->c, read_u64(va->c, va->va_list + 16) + fp_offset);
            fp_offset += 16;
            gmem_write(va->c->mem, va->va_list + 4, &fp_offset, 4);
        } else {
            guest_addr area = read_u64(va->c, va->va_list + 8);
            bits = read_u64(va->c, area);
            gmem_write_u64(va->c->mem, va->va_list + 8, area + 8);
        }
    }
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

typedef struct out_buf {
    char *p;
    size_t size, len;
} out_buf;

static void put(out_buf *o, const char *s, size_t n) {
    if (o->len < o->size) {
        size_t room = o->size - o->len;
        memcpy(o->p + o->len, s, n < room ? n : room);
    }
    o->len += n;
}

static int guest_char(hle_call *c, guest_addr addr) {
    const char *p = (const char *)gmem_host(c->mem, addr, 1);
    return p ? (unsigned char)*p : -1;
}

static void pad_string(out_buf *o, size_t count) {
    if(o->len<o->size) {
        size_t room=o->size-o->len;
        memset(o->p+o->len,' ',count<room?count:room);
    }
    o->len+=count;
}

/* String arguments can be larger than the numeric conversion scratch buffer.
 * Scan mapped pages and honor precision before touching the next byte. */
static void format_string(hle_call *c,out_buf *o,guest_addr addr,const char *spec) {
    const char *p=spec+1;bool left=false;
    while(*p && strchr("-+ #0",*p)){left|=*p=='-';p++;}
    size_t width=0,limit=SIZE_MAX;
    while(*p>='0' && *p<='9'){width=width>(INT32_MAX-9u)/10u?INT32_MAX:width*10u+(unsigned)(*p-'0');p++;}
    if(*p=='.') {
        p++;limit=0;
        while(*p>='0' && *p<='9'){limit=limit>(INT32_MAX-9u)/10u?INT32_MAX:limit*10u+(unsigned)(*p-'0');p++;}
    }
    size_t n=0;
    if(!addr)n=limit<6?limit:6;
    else while(n<limit) {
        size_t part=GUEST_PAGE_SIZE-(size_t)((addr+n)%GUEST_PAGE_SIZE);
        if(part>limit-n)part=limit-n;
        const char *s=gmem_host(c->mem,addr+n,part);
        if(!s)break;
        const char *end=memchr(s,0,part);
        if(end){n+=(size_t)(end-s);break;}
        n+=part;
    }
    size_t padding=width>n?width-n:0;
    if(!left)pad_string(o,padding);
    if(!addr)put(o,"(null)",n);
    else for(size_t at=0;at<n;) {
        size_t part=GUEST_PAGE_SIZE-(size_t)((addr+at)%GUEST_PAGE_SIZE);
        if(part>n-at)part=n-at;
        const char *s=gmem_host(c->mem,addr+at,part);
        if(!s)break;
        put(o,s,part);at+=part;
    }
    if(left)pad_string(o,padding);
}

size_t guest_format(hle_call *c, guest_addr fmt, guest_va *va, char *out, size_t out_size) {
    out_buf o = {out, out_size ? out_size - 1 : 0, 0};
    char tmp[512];
    for (guest_addr f = fmt;;) {
        int ch = guest_char(c, f++);
        if (ch <= 0) break;
        if (ch != '%') {
            char cc = (char)ch;
            put(&o, &cc, 1);
            continue;
        }
        /* Collect "%[flags][width][.prec]" into a host spec, resolving '*'. */
        char spec[64];
        size_t sl = 0;
        spec[sl++] = '%';
        ch = guest_char(c, f++);
        while (ch > 0 && strchr("-+ #0", ch) && sl < 8) {
            spec[sl++] = (char)ch;
            ch = guest_char(c, f++);
        }
        for (int part = 0; part < 2; part++) {
            if (part == 1) {
                if (ch != '.') break;
                spec[sl++] = '.';
                ch = guest_char(c, f++);
            }
            if (ch == '*') {
                sl += (size_t)snprintf(spec + sl, sizeof(spec) - sl, "%d", (int)(int32_t)guest_va_int(va));
                ch = guest_char(c, f++);
            } else {
                while (ch >= '0' && ch <= '9' && sl < 24) {
                    spec[sl++] = (char)ch;
                    ch = guest_char(c, f++);
                }
            }
        }
        /* Length modifier */
        int longness = 0; /* 0 int, 1 long/64-bit, -1 short, -2 char */
        for (;;) {
            if (ch == 'l' || ch == 'z' || ch == 'j' || ch == 't' || ch == 'q' || ch == 'L') longness = 1;
            else if (ch == 'h') longness = longness == -1 ? -2 : -1;
            else break;
            ch = guest_char(c, f++);
        }
        if (ch <= 0) break;
        int n = 0;
        switch (ch) {
        case '%': put(&o, "%", 1); continue;
        case 'd':
        case 'i': {
            uint64_t v = guest_va_int(va);
            long long s = longness == 1 ? (long long)(int64_t)v
                          : longness == -1 ? (long long)(int16_t)v
                          : longness == -2 ? (long long)(int8_t)v
                                           : (long long)(int32_t)v;
            memcpy(spec + sl, "lld", 4);
            n = snprintf(tmp, sizeof(tmp), spec, s);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v = guest_va_int(va);
            unsigned long long u = longness == 1 ? (unsigned long long)v
                                   : longness == -1 ? (unsigned long long)(uint16_t)v
                                   : longness == -2 ? (unsigned long long)(uint8_t)v
                                                    : (unsigned long long)(uint32_t)v;
            spec[sl] = 'l';
            spec[sl + 1] = 'l';
            spec[sl + 2] = (char)ch;
            spec[sl + 3] = '\0';
            n = snprintf(tmp, sizeof(tmp), spec, u);
            break;
        }
        case 'c': {
            spec[sl] = 'c';
            spec[sl + 1] = '\0';
            n = snprintf(tmp, sizeof(tmp), spec, (int)(uint8_t)guest_va_int(va));
            break;
        }
        case 'p': {
            n = snprintf(tmp, sizeof(tmp), "0x%llx", (unsigned long long)guest_va_int(va));
            break;
        }
        case 's': {
            guest_addr s = guest_va_int(va);
            spec[sl] = '\0';
            format_string(c,&o,s,spec);
            continue;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            spec[sl] = (char)ch;
            spec[sl + 1] = '\0';
            n = snprintf(tmp, sizeof(tmp), spec, guest_va_double(va));
            break;
        }
        case 'n': guest_va_int(va); continue; /* never write through %n */
        default: {
            char bad[2] = {'%', (char)ch};
            put(&o, bad, 2);
            continue;
        }
        }
        if (n > 0) put(&o, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
    if (out_size) out[o.len < o.size ? o.len : o.size] = '\0';
    return o.len;
}
