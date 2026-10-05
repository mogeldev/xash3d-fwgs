/*
mod_nfspz.c - James Bond 007: Nightfire (PC) "SPZ2" sprite -> GoldSrc sprite.

Nightfire sprites are pure metadata: a header plus per-frame PNG file names
(pixels live under sprites/textures/). GoldSrc/Xash3D sprites embed their pixels,
so this module decodes each referenced PNG (through a caller-supplied loader)
and builds a GoldSrc RGBA sprite (SPRITE_VERSION_32).

SPZ2 layout (see tools/nf_spz.py):
    char magic[4] = "SPZ2"; uint8 flags; uint8 reserved[2]; uint8 numframes;
    frame[numframes]: char name[32]; uint8 payload[36] (unused)

GoldSrc v32 sprite layout:
    dsprite_q1_t header (36 bytes)
    frame[numframes]: int32 frametype(0); int32 origin[2]; int32 w,h; RGBA[w*h]

Copyright (C) 2026 Nightfire-Xash3D port.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef NFSPZ_STANDALONE
typedef unsigned char byte;
typedef int qboolean;
#define true 1
#define false 0
typedef int (*nfspz_loadfn)( const char *name, int *w, int *h, unsigned char **rgba );
qboolean NFSPZ_IsVersion2( const void *buffer, size_t size );
byte *NFSPZ_Convert2( const void *buffer, size_t size, nfspz_loadfn load, size_t *outsize );
#else
#include "mod_nfspz.h"
#endif

#define IDSPRITEHEADER (('P'<<24)+('S'<<16)+('D'<<8)+'I')
#define SPRITE_VERSION_32 32
#define FRAME_SINGLE 0
#define SPR_FWD_PARALLEL 2

#define SPZ_HDR 8
#define SPZ_REC 68

qboolean NFSPZ_IsVersion2( const void *buffer, size_t size )
{
	const unsigned char *p = (const unsigned char *)buffer;
	if( !p || size < SPZ_HDR ) return false;
	return p[0] == 'S' && p[1] == 'P' && p[2] == 'Z' && p[3] == '2';
}

typedef struct { unsigned char *d; size_t len, cap; } buf_t;
static int buf_reserve( buf_t *b, size_t extra )
{
	if( b->len + extra <= b->cap ) return 1;
	size_t ncap = b->cap ? b->cap : 4096;
	while( ncap < b->len + extra ) ncap *= 2;
	unsigned char *nd = (unsigned char *)realloc( b->d, ncap );
	if( !nd ) return 0;
	b->d = nd; b->cap = ncap; return 1;
}
static void buf_bytes( buf_t *b, const void *src, size_t n )
{
	if( n == 0 ) return;
	if( !buf_reserve( b, n )) return;
	memcpy( b->d + b->len, src, n ); b->len += n;
}
static void buf_i32( buf_t *b, int v ) { buf_bytes( b, &v, 4 ); }

byte *NFSPZ_Convert2( const void *buffer, size_t size, nfspz_loadfn load, size_t *outsize )
{
	const unsigned char *in = (const unsigned char *)buffer;
	if( !load || !NFSPZ_IsVersion2( buffer, size )) return NULL;

	int numframes = in[7];
	if( numframes <= 0 || (size_t)( SPZ_HDR + numframes * SPZ_REC ) > size )
		return NULL;

	// load every frame first (so we can compute bounds and header)
	unsigned char **pix = (unsigned char **)calloc( numframes, sizeof( unsigned char * ));
	int *fw = (int *)calloc( numframes, sizeof( int ));
	int *fh = (int *)calloc( numframes, sizeof( int ));
	if( !pix || !fw || !fh ) { free( pix ); free( fw ); free( fh ); return NULL; }

	int maxw = 0, maxh = 0;
	for( int i = 0; i < numframes; i++ )
	{
		char name[40];
		const unsigned char *rec = in + SPZ_HDR + (size_t)i * SPZ_REC;
		int k = 0;
		while( k < 32 && rec[k] ) { name[k] = (char)rec[k]; k++; }
		name[k] = '\0';
		if( !load( name, &fw[i], &fh[i], &pix[i] ) || !pix[i] || fw[i] <= 0 || fh[i] <= 0 )
		{
			fw[i] = fh[i] = 0;	// skip but keep frame (empty)
			if( pix[i] ) { free( pix[i] ); pix[i] = NULL; }
			continue;
		}
		if( fw[i] > maxw ) maxw = fw[i];
		if( fh[i] > maxh ) maxh = fh[i];
	}
	if( maxw <= 0 ) maxw = 1;
	if( maxh <= 0 ) maxh = 1;

	buf_t out = { 0 };
	// dsprite_q1_t
	unsigned char hdr[36];
	memset( hdr, 0, sizeof( hdr ));
	int ident = IDSPRITEHEADER, version = SPRITE_VERSION_32, type = SPR_FWD_PARALLEL;
	int bounds[2] = { maxw, maxh };
	float radius = (float)sqrt( (double)maxw * maxw + (double)maxh * maxh ) * 0.5f;
	memcpy( hdr + 0, &ident, 4 );
	memcpy( hdr + 4, &version, 4 );
	memcpy( hdr + 8, &type, 4 );
	memcpy( hdr + 12, &radius, 4 );
	memcpy( hdr + 16, bounds, 8 );
	memcpy( hdr + 24, &numframes, 4 );
	// beamlength (28) = 0, synctype (32) = 0
	buf_bytes( &out, hdr, 36 );

	for( int i = 0; i < numframes; i++ )
	{
		int ft = FRAME_SINGLE;
		buf_i32( &out, ft );
		int origin[2] = { 0, 0 };
		buf_bytes( &out, origin, 8 );
		buf_i32( &out, fw[i] );
		buf_i32( &out, fh[i] );
		if( pix[i] && fw[i] > 0 && fh[i] > 0 )
			buf_bytes( &out, pix[i], (size_t)fw[i] * fh[i] * 4 );
		free( pix[i] );
	}

	free( pix ); free( fw ); free( fh );
	*outsize = out.len;
	return (byte *)out.d;
}
