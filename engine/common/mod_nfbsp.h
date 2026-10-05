/*
mod_nfbsp.h - James Bond 007: Nightfire (PC) BSP42 compatibility layer.

Nightfire's PC engine is a heavily modified GoldSrc whose maps use BSP version
42 with 18 lumps and a much richer surface/brush model than GoldSrc's BSP30.
Xash3D's renderer, collision and visibility code all operate on BSP30 data, so
rather than teach every subsystem a second format we translate a BSP42 in memory
into an equivalent BSP30 immediately before the standard loader runs. This keeps
the whole proven BSP30 pipeline intact (rendering, PVS, hulls, lightmaps).

The translation and its limitations are documented in tools/nfbsp42_to_bsp30.py,
which is the Python reference implementation this module mirrors.

Copyright (C) 2026 Nightfire-Xash3D port.
*/
#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NIGHTFIRE_BSP_VERSION 42

// Returns true if the buffer begins with a Nightfire BSP42 header.
qboolean NFBSP_IsVersion42( const void *buffer, size_t size );

// Translates a BSP42 buffer into a freshly malloc'd BSP30 buffer.
// Returns NULL on failure. The caller owns (and must free) the result.
byte *NFBSP_Convert42( const void *buffer, size_t size, size_t *outsize );

#ifdef __cplusplus
}
#endif
