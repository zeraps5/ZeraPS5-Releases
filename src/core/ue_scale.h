/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Internal resolution of Unreal Engine games ([geral] escala_unreal=<percent>).
 *
 * Unreal titles render their PS5 output at 4K and pick the scene's size from the
 * PS5 device profile in their .pak (Little Nightmares: FSR at 77 %, Dragon Ball
 * Sparking Zero: dynamic resolution up to 100 % then 75 %). With the setting on,
 * the profile is read from the game's own pak, its screen-percentage, dynamic
 * resolution and upscaler quality lines are replaced by a fixed
 * r.ScreenPercentage, and the result goes into a small pak in the mods folder
 * (user/mods/<title id>/zeraps5-escala/<project>/Saved/Paks), which the game
 * mounts over its own files. */
#ifndef PS5C_CORE_UE_SCALE_H
#define PS5C_CORE_UE_SCALE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Writes or removes the generated pak before the mods are listed. app_dir is the
 * game's folder (/app0), title_id a checked title ID. */
void ue_scale_prepare(const char *app_dir, const char *title_id);

/* The device profile ini with the scale lines of every "... DeviceProfile]" section
 * replaced (exposed for the unit test). Returns the length written, 0 if out is too
 * small. */
size_t ue_scale_profile(const char *in, size_t n, int percent, char *out, size_t cap);

/* A pak (version 7, uncompressed) holding one file; malloc'd, *size set. */
uint8_t *ue_scale_pak(const char *name, const void *data, size_t n, size_t *size);

#endif
