/*
mod_nfmdl.h - James Bond 007: Nightfire (PC) studio model (MDLZ v14)
compatibility layer.

Nightfire's PC engine is a heavily modified GoldSrc whose studio models use
magic "MDLZ", version 14. Xash3D only understands GoldSrc studio MDL v10, so
an MDLZ buffer is translated in memory right before the standard loader runs.

See mod_nfmdl.c and tools/nf_mdl.py / tools/nf_mdl10.py.

Copyright (C) 2026 Nightfire-Xash3D port.
*/
#pragma once

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns true if the buffer begins with a Nightfire MDLZ v14 header.
qboolean NFMDL_IsVersion14( const void *buffer, size_t size );

// Translates an MDLZ v14 buffer into a freshly malloc'd GoldSrc MDL v10 buffer.
// Returns NULL on failure. The caller owns (and must free) the result.
byte *NFMDL_Convert14( const void *buffer, size_t size, size_t *outsize );

#ifdef __cplusplus
}
#endif
