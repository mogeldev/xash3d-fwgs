/*
mod_nfspz.h - James Bond 007: Nightfire (PC) sprite (SPZ2) compatibility layer.

Nightfire sprites ("SPZ2") are metadata referencing external PNG frames. Xash3D
sprites embed their pixels, so an SPZ2 is converted to a GoldSrc RGBA sprite
(SPRITE_VERSION_32) after decoding the PNGs through a caller-supplied loader.

See mod_nfspz.c and tools/nf_spz.py.

Copyright (C) 2026 Nightfire-Xash3D port.
*/
#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loads a sprite frame PNG by base name (e.g. "energyball0000.png").
// On success returns true and sets *w,*h,*rgba, where *rgba is a malloc'd
// w*h*4 RGBA buffer owned (and freed) by the converter. On failure returns
// false; *rgba may be left untouched.
typedef int (*nfspz_loadfn)( const char *name, int *w, int *h, unsigned char **rgba );

// Returns true if the buffer begins with an SPZ2 header.
qboolean NFSPZ_IsVersion2( const void *buffer, size_t size );

// Translates an SPZ2 buffer into a freshly malloc'd GoldSrc sprite buffer.
// Returns NULL on failure. The caller owns (and must free) the result.
byte *NFSPZ_Convert2( const void *buffer, size_t size, nfspz_loadfn load, size_t *outsize );

#ifdef __cplusplus
}
#endif
