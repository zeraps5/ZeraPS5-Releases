/* SPDX-License-Identifier: GPL-2.0-only */
/* Streaming cubic resampling adapted from boykopovar/AnyPS5, Ajm.cpp
 * (bf6e25a1). Ratio is input frames per output frame. Keep two lookahead
 * frames and interpolation history across jobs; clearing keeps the ratio.
 * This is a software approximation, not a measured PS5 filter response. */
#ifndef PS5C_AJM_RESAMPLE_H
#define PS5C_AJM_RESAMPLE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct ajm_resampler {
    float ratio;
    double position;
    double *pcm;
    size_t count, capacity;
    uint32_t channels;
} ajm_resampler;

static inline void ajm_resample_clear(ajm_resampler *s) { s->count = 0; s->position = 1.0; }
static inline void ajm_resample_free(ajm_resampler *s) {
    free(s->pcm); s->pcm = NULL; s->capacity = 0; s->channels = 0;
    ajm_resample_clear(s);
}
static inline bool ajm_resample_active(const ajm_resampler *s) { return s->ratio != 1.0f || s->count != 0; }
static inline size_t ajm_resample_held(const ajm_resampler *s) {
    return s->position < (double)s->count ? s->count - (size_t)s->position : 0;
}
static inline bool ajm_resample_ready(const ajm_resampler *s) { return s->position + 2.0 < (double)s->count; }

static inline bool ajm_resample_push(ajm_resampler *s, const uint8_t *pcm, size_t frames,
                             uint32_t channels, uint32_t encoding) {
    if (!frames) return true;
    if (!pcm || !channels || channels > 8 || encoding > 2 || (s->count && s->channels != channels)) return false;
    size_t prefix = s->count == 0 ? 1 : 0;
    /* A job that cannot drain stops decoding; bounded FIFO as a last guard. */
    if (frames > 262144 - prefix || s->count > 262144 - frames - prefix) return false;
    size_t needed = s->count + frames + prefix;
    if (s->capacity < needed || s->channels != channels) {
        size_t capacity = needed;
        double *p = realloc(s->pcm, capacity * channels * sizeof(double));
        if (!p) return false;
        s->pcm = p; s->capacity = capacity; s->channels = channels;
    }
    size_t bytes = encoding == 0 ? 2 : 4;
    double *at = s->pcm + (s->count + prefix) * channels;
    for (size_t i = 0; i < frames * channels; ++i) {
        if (encoding == 0) { int16_t v; memcpy(&v, pcm + i * bytes, 2); at[i] = v / 32768.0; }
        else if (encoding == 1) { int32_t v; memcpy(&v, pcm + i * bytes, 4); at[i] = v / 2147483648.0; }
        else { float v; memcpy(&v, pcm + i * bytes, 4); at[i] = v; }
    }
    if (prefix) memcpy(s->pcm, at, channels * sizeof(double));
    s->count = needed;
    return true;
}

static inline void ajm_resample_store(double v, uint8_t *out, uint32_t encoding) {
    if (encoding == 2) { float f = (float)v; memcpy(out, &f, 4); return; }
    if (!isfinite(v)) v = 0;
    if (v > 1.0) v = 1.0;
    if (v < -1.0) v = -1.0;
    if (encoding == 0) {
        long long n = llrint(v * 32768.0);
        int16_t x = (int16_t)(n > 32767 ? 32767 : n);
        memcpy(out, &x, 2);
    } else {
        long long n = llrint(v * 2147483648.0);
        int32_t x = (int32_t)(n > INT32_MAX ? INT32_MAX : n);
        memcpy(out, &x, 4);
    }
}

static inline uint64_t ajm_resample_produce(ajm_resampler *s, uint8_t *out, uint64_t room, uint32_t encoding) {
    if (!out || !s->count || !s->channels || !isfinite(s->ratio) || s->ratio <= 0) return 0;
    size_t bytes = encoding == 0 ? 2 : 4, stride = s->channels * bytes;
    uint64_t written = 0;
    while (room - written >= stride && ajm_resample_ready(s)) {
        size_t index = (size_t)s->position;
        double t = s->position - (double)index;
        for (uint32_t c = 0; c < s->channels; ++c) {
            double p0 = s->pcm[(index - 1) * s->channels + c], p1 = s->pcm[index * s->channels + c];
            double p2 = s->pcm[(index + 1) * s->channels + c], p3 = s->pcm[(index + 2) * s->channels + c];
            double v = p1 + 0.5 * t * (p2 - p0 + t * (2*p0 - 5*p1 + 4*p2 - p3 + t * (3*(p1-p2) + p3-p0)));
            ajm_resample_store(v, out + written + c * bytes, encoding);
        }
        written += stride;
        s->position += s->ratio;
    }
    /* Compare before converting to size_t: finite float ratios can exceed
     * SIZE_MAX. Retain the future skip in position without an invalid cast. */
    size_t index = s->position >= (double)s->count ? s->count : (size_t)s->position;
    if (index > 1) {
        size_t drop = index - 1;
        memmove(s->pcm, s->pcm + drop * s->channels, (s->count - drop) * s->channels * sizeof(double));
        s->count -= drop; s->position -= (double)drop;
    }
    return written;
}
#endif
