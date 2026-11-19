/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "pal/pal.h"

void pal_host_get_info(pal_host_info *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->cpu, sizeof(out->cpu), "x86-64");
    snprintf(out->os, sizeof(out->os), "%s", pal_os_name());
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count > 0) out->logical_processors = (uint32_t)count;
    struct utsname name;
    if (uname(&name) == 0) snprintf(out->os, sizeof(out->os), "%.24s %.48s %.16s", name.sysname, name.release, name.machine);
#if defined(__linux__)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "model name", 10)) continue;
            char *text = strchr(line, ':');
            if (text) {
                text++;
                while (*text == ' ' || *text == '\t') text++;
                text[strcspn(text, "\r\n")] = 0;
                snprintf(out->cpu, sizeof(out->cpu), "%.95s", text);
            }
            break;
        }
        fclose(f);
    }
#endif
}

void pal_host_get_stats(pal_host_stats *out) {
    memset(out, 0, sizeof(*out));
#if defined(__linux__)
    FILE *f = fopen("/proc/meminfo", "r");
    bool have_total = false, have_available = false;
    if (f) {
        char line[256];
        unsigned long long kb;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) { out->ram_total = kb * 1024; have_total = true; }
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) { out->ram_available = kb * 1024; have_available = true; }
        }
        fclose(f);
        if (have_total && have_available) out->valid |= PAL_HOST_RAM;
    }
    f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long long virtual_pages, resident;
        long page = sysconf(_SC_PAGESIZE);
        if (page > 0 && fscanf(f, "%llu %llu", &virtual_pages, &resident) == 2) {
            out->process_ram = resident * (uint64_t)page;
            out->valid |= PAL_HOST_PROCESS_RAM;
            /* Commit is unavailable: virtual size is not committed memory. */
        }
        fclose(f);
    }
#endif
#if defined(CLOCK_PROCESS_CPUTIME_ID)
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) == 0) {
        out->process_cpu_ns = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
        out->valid |= PAL_HOST_PROCESS_CPU;
    }
#endif
}
