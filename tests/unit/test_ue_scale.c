/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unreal internal resolution (core/ue_scale.h): the device profile keeps its other
 * lines, loses the scale ones and gains a fixed screen percentage; the pak written
 * holds the file after its header and ends with a version 7 footer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/sha1.h"
#include "core/ue_scale.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int main(void) {
    const char *in = "[PS5 DeviceProfile]\r\nDeviceType=PS5\r\n+CVars=r.SIE.PSSR.enable=0\r\n+CVars=r.fidelityfx.fsr.quality=4\r\n"
                     "+CVars=r.DynamicRes.OperationMode=2\r\n+CVars=r.SecondaryScreenPercentage.GameViewport=75\r\n"
                     "+CVars=r.ScreenPercentage=77\r\n-CVars=r.RayTracing.Reflections.ScreenPercentage=50\r\n\r\n"
                     "[Other]\r\n+CVars=r.ScreenPercentage=80\r\n";
    char out[1024];
    size_t n = ue_scale_profile(in, strlen(in), 50, out, sizeof(out));
    CHECK(n && n == strlen(out));
    CHECK(strstr(out, "[PS5 DeviceProfile]\r\n+CVars=r.DynamicRes.OperationMode=0\r\n+CVars=r.ScreenPercentage=50\r\n"));
    CHECK(strstr(out, "DeviceType=PS5\r\n") && strstr(out, "r.SIE.PSSR.enable=0"));
    CHECK(!strstr(out, "fsr.quality") && !strstr(out, "OperationMode=2") && !strstr(out, "GameViewport"));
    CHECK(!strstr(out, "ScreenPercentage=77") && strstr(out, "-CVars=r.RayTracing.Reflections.ScreenPercentage=50\r\n"));
    CHECK(strstr(out, "[Other]\r\n+CVars=r.ScreenPercentage=80")); /* not a device profile: untouched */
    CHECK(ue_scale_profile(in, strlen(in), 50, out, 40) == 0);

    /* a header on the last line still gets its lines */
    const char *last = "[PS5_Trinity DeviceProfile]";
    n = ue_scale_profile(last, strlen(last), 67, out, sizeof(out));
    CHECK(n && !strcmp(out, "[PS5_Trinity DeviceProfile]\r\n+CVars=r.DynamicRes.OperationMode=0\r\n+CVars=r.ScreenPercentage=67\r\n"));

    size_t size = 0;
    const char *data = "hello";
    uint8_t *pak = ue_scale_pak("Atlas/Config/PS5/PS5DeviceProfiles.ini", data, 5, &size);
    CHECK(pak && size > 61);
    CHECK(!memcmp(pak + 53, "hello", 5));                       /* after the 53-byte entry header */
    const uint8_t *foot = pak + size - 61;
    CHECK(rd32(foot + 17) == 0x5A6F12E1u && rd32(foot + 21) == 7 && foot[16] == 0);
    uint32_t index_at = rd32(foot + 25), index_size = rd32(foot + 33);
    CHECK(index_at == 58 && index_at + index_size == size - 61);
    uint8_t hash[20];
    sha1(pak + index_at, index_size, hash);
    CHECK(!memcmp(hash, foot + 41, 20));
    CHECK(rd32(pak + index_at) == 10 && !memcmp(pak + index_at + 4, "../../../", 10));
    free(pak);
    printf("ue scale: ok\n");
    return 0;
}
