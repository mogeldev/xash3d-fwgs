/*
mod_nfmdl.c - James Bond 007: Nightfire (PC) "MDLZ" v14 -> GoldSrc studio MDL v10.

Nightfire's PC engine is a heavily modified GoldSrc. Its studio models use
magic "MDLZ", version 14, with a flat little-endian layout that stores geometry
once, globally, as parallel arrays and uses count/offset pairs per section.

Xash3D's studio pipeline only understands GoldSrc MDL v10 ("IDST", 10), so this
module translates an MDLZ buffer in memory into an equivalent v10 buffer
immediately before the standard loader runs. It is self-contained (libc only)
so it can be unit-tested outside the engine (see work/nfmdl_selftest.c).

Reference: nightfire-open/tools/mdl-decompiler (MDLv14 ComponentReader) and
tools/nf_mdl.py / tools/nf_mdl10.py.

Conversion notes (v1):
  - header: name, eye position, movement hull (boundingBox) and clipping hull
    (clippingBox) are copied.
  - bones/controllers/hitboxes: copied 1:1 (units match GoldSrc).
  - sequences: descriptions mapped 1:1; the compressed animation block is
    byte-copied (identical RLE: mstudioanim_t.offset[6] + {valid,total}, laid
    out [blend][bone]). All retail sequences are seqgroup 0 (inline).
  - geometry: each submodel gets a compact vertex/normal array built from the
    global arrays (retail submodels partition the global vertices).
    Winding and V are used as-is.
  - one bone per vertex (blend slot 0); 4-bone skinning is not emitted yet.
  - textures carry the Nightfire texture name; the renderer is expected to
    resolve "models/textures/<name>.png". UVs use a fixed 64 texels/unit scale.

GoldSrc requires all variable-size arrays to be contiguous:
  seqdesc[i]   at seqindex   + i*176
  bodypart[i]  at bodypartindex + i*76
  model[j]     at modelindex  + j*112
  mesh[k]      at meshindex   + k*20
so this converter emits those arrays first and appends the animation / geometry
blobs afterwards, patching the index fields.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef NFMDL_STANDALONE
typedef unsigned char byte;
typedef int qboolean;
#define true 1
#define false 0
qboolean NFMDL_IsVersion14( const void *buffer, size_t size );
byte *NFMDL_Convert14( const void *buffer, size_t size, size_t *outsize );
#else
#include "mod_nfmdl.h"
#endif

#define IDSTUDIOHEADER  (('T'<<24)+('S'<<16)+('D'<<8)+'I')
#define STUDIO_VERSION  10
#define UVSCALE         64
#define MAXSTUDIOVERTS  16384
#define MAXSTUDIOBONES  128
#define MAXSTUDIOMESHES 64

#define STUDIO_NF_CHROME     0x0002
#define STUDIO_NF_FULLBRIGHT 0x0004
#define STUDIO_NF_ADDITIVE   0x0020
#define STUDIO_NF_MASKED     0x0040

enum {
	H_VERSION = 4, H_NAME = 8, H_EYE = 76, H_MIN = 88, H_MAX = 100,
	H_BBMIN = 112, H_BBMAX = 124, H_BONES = 140, H_BONECTL = 148, H_HITBOX = 156,
	H_SEQ = 164, H_TEX = 180, H_SKINREF = 192, H_SKINFAM = 196, H_SKINS = 200,
	H_BODYGRP = 204, H_ATTACH = 212,
	H_VERTCOUNT = 252, H_TRICOUNT = 256, H_TRIMAP = 260,
	H_VERTS = 264, H_NORMS = 268, H_TEXCO = 272, H_BLENDSCALE = 280, H_BLENDING = 284,
	H_BONEFIX = 288,
	H_SIZE = 484
};

#define SZ_BONE      112
#define SZ_BONECTL    24
#define SZ_HITBOX     32
#define SZ_SEQ       188
#define SZ_TEX       136
#define SZ_BODYGRP    76
#define SZ_ATTACH     88
#define SZ_MESH       32
#define SZ_MODEL     132
#define SZ_EVENT      76
#define NUM_MODELINFOS 24

static int rd_i32( const unsigned char *p )
{
	return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) |
		((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}
static unsigned rd_u16( const unsigned char *p )
{
	return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static float rd_f32( const unsigned char *p )
{
	int i = rd_i32( p ); float f; memcpy( &f, &i, 4 ); return f;
}
static void nf_cstr( const unsigned char *p, int maxlen, char *out, int outsz )
{
	int i = 0;
	while( i < maxlen && p[i] && i < outsz - 1 ) { out[i] = (char)p[i]; i++; }
	out[i] = '\0';
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
static void buf_u16( buf_t *b, unsigned short v ) { buf_bytes( b, &v, 2 ); }
static void buf_f32( buf_t *b, float v ) { buf_bytes( b, &v, 4 ); }
static void buf_u8( buf_t *b, unsigned char v ) { buf_bytes( b, &v, 1 ); }
static void buf_zero( buf_t *b, size_t n ) { if( buf_reserve( b, n )) { memset( b->d + b->len, 0, n ); b->len += n; } }
static void buf_patch_i32( buf_t *b, size_t at, int v ) { memcpy( b->d + at, &v, 4 ); }
static void buf_align4( buf_t *b ) { while( b->len & 3 ) buf_u8( b, 0 ); }

qboolean NFMDL_IsVersion14( const void *buffer, size_t size )
{
	const unsigned char *p = (const unsigned char *)buffer;
	if( !p || size < H_SIZE ) return false;
	return p[0] == 'M' && p[1] == 'D' && p[2] == 'L' && p[3] == 'Z' && rd_i32( p + H_VERSION ) == 14;
}

static size_t nf_anim_block_len( const unsigned char *in, size_t size, int start,
	int numbones, int blends, int numframes )
{
	if( start <= 0 || !numbones || !blends || numframes <= 0 ) return 0;
	size_t maxend = (size_t)start + (size_t)numbones * blends * 12;
	if( maxend > size ) return 0;
	for( int b = 0; b < blends; b++ )
		for( int bone = 0; bone < numbones; bone++ )
		{
			size_t entry = (size_t)start + ((size_t)b * numbones + bone) * 12;
			if( entry + 12 > size ) return 0;
			for( int axis = 0; axis < 6; axis++ )
			{
				unsigned off = rd_u16( in + entry + axis * 2 );
				if( !off ) continue;
				size_t p = entry + off;
				int frames = 0;
				while( frames < numframes )
				{
					if( p + 2 > size ) return 0;
					unsigned v = rd_u16( in + p );
					int valid = (int)( v & 0xff ), total = (int)( v >> 8 );
					if( total <= 0 ) break;
					p += 2 + (size_t)valid * 2;
					frames += total;
				}
				if( p > maxend ) maxend = p;
			}
		}
	if( maxend > size ) return 0;
	return maxend - (size_t)start;
}

// Emit the mesh array + tri commands + vertex/normal arrays for one submodel.
// The mstudiomodel_t record is filled in-place.
// Build one mstudioboneweight_t. Valid bones are packed into slots 0..k-1
// (the renderer walks bone[0..count-1]) and weights are normalised to sum 255.
static void nf_make_boneweight( const unsigned char *in, size_t size, int g,
	int *gbone, int *gbone4, int scales_off, unsigned char w4[4], signed char b4[4] )
{
	int k = 0, sum = 0;

	w4[0] = w4[1] = w4[2] = w4[3] = 0;
	b4[0] = b4[1] = b4[2] = b4[3] = -1;

	for( int s = 0; s < 4 && k < 4; s++ )
	{
		signed char bone = (signed char)gbone4[(size_t)g * 4 + s];
		float sc = 0.0f;
		int wi;

		if( bone < 0 ) continue;
		if( scales_off >= 0 && scales_off + (size_t)g * 16 + s * 4 + 4 <= size )
			sc = rd_f32( in + scales_off + (size_t)g * 16 + s * 4 );
		wi = (int)( sc * 255.0f + 0.5f );
		if( wi < 0 ) wi = 0;
		if( wi > 255 ) wi = 255;
		b4[k] = bone;
		w4[k] = (unsigned char)wi;
		sum += wi;
		k++;
	}

	if( k == 0 )
	{
		b4[0] = (signed char)gbone[g];
		w4[0] = 255;
		return;
	}
	if( sum <= 0 )
	{
		w4[0] = 255;
		w4[1] = w4[2] = w4[3] = 0;
		return;
	}
	if( sum != 255 )
	{
		int acc = 0, d, nv;

		for( int s = 0; s < k; s++ )
		{
			int v = (int)( (float)w4[s] * 255.0f / (float)sum + 0.5f );
			if( v < 0 ) v = 0; if( v > 255 ) v = 255;
			w4[s] = (unsigned char)v; acc += v;
		}
		d = 255 - acc;
		nv = (int)w4[0] + d;
		if( nv < 0 ) nv = 0; if( nv > 255 ) nv = 255;
		w4[0] = (unsigned char)nv;
	}
}

static void emit_model_geometry( buf_t *out, const unsigned char *in, size_t size,
	size_t modelrec, size_t mp_off,
	int nverts_g, int ntris_g, int trimap_off, int verts_off, int norms_off,
	int texco_off, int blends_off, int scales_off, int use_weights,
	int *gmap, int *gbone, int *gbone4, int *local2g )
{
	const unsigned char *mp = in + mp_off;

	memset( gmap, 0xff, (size_t)(nverts_g > 0 ? nverts_g : 1) * sizeof( int ));
	int nextlocal = 0, meshcount = 0;
	// Local vertex/normal indices are handed out mesh by mesh, so every mesh
	// introduces a contiguous run of new normals. The renderer lights normals
	// sequentially per mesh (mstudiomesh_t.numnorms) and the tri commands index
	// those light values, so each mesh must report how many it introduced;
	// with 0 every vertex stays unlit (black).
	int mesh_norms[MAXSTUDIOMESHES];
	memset( mesh_norms, 0, sizeof( mesh_norms ));
	for( int mii = 0; mii < NUM_MODELINFOS; mii++ )
	{
		int mio = rd_i32( mp + 36 + 4 * mii );
		if( !mio ) continue;
		int mc = rd_i32( in + mio + 4 ), moff = rd_i32( in + mio + 8 );
		for( int k = 0; k < mc; k++ )
		{
			const unsigned char *mesh = in + moff + (size_t)k * SZ_MESH;
			int tcount = rd_u16( mesh + 26 ), tindex = rd_u16( mesh + 24 );
			const int mslot = meshcount < MAXSTUDIOMESHES ? meshcount : MAXSTUDIOMESHES - 1;
			const int firstlocal = nextlocal;
			meshcount++;
			for( int t = 0; t < tcount; t++ )
			{
				int ti = tindex + t;
				if( ti < 0 || ti >= ntris_g ) continue;
				int gv = rd_u16( in + trimap_off + (size_t)ti * 2 );
				if( gv < 0 || gv >= nverts_g || gmap[gv] >= 0 ) continue;
				gmap[gv] = nextlocal++;
				signed char v0 = (signed char)in[blends_off + (size_t)gv * 4];
				int bone = ( v0 >= 0 ) ? (signed char)mesh[v0] : 0;
				gbone[gv] = ( bone < 0 ) ? 0 : bone;
				if( use_weights )
				{
					for( int k = 0; k < 4; k++ )
					{
						signed char bk = (signed char)in[blends_off + (size_t)gv * 4 + k];
						gbone4[(size_t)gv * 4 + k] = ( bk >= 0 ) ? (signed char)mesh[bk] : -1;
					}
				}
			}
			// meshes past the limit are not emitted; their normals go to the last one
			mesh_norms[mslot] += nextlocal - firstlocal;
		}
	}
	if( meshcount > MAXSTUDIOMESHES ) meshcount = MAXSTUDIOMESHES;
	for( int g = 0; g < nverts_g; g++ ) if( gmap[g] >= 0 ) local2g[gmap[g]] = g;

	// fill mstudiomodel_t fields
	char mn[64]; memset( mn, 0, 64 ); nf_cstr( mp, 32, mn, 64 );
	memcpy( out->d + modelrec, mn, 64 );
	buf_patch_i32( out, modelrec + 64, 0 );
	{ float z = 0.0f; memcpy( out->d + modelrec + 68, &z, 4 ); }
	buf_patch_i32( out, modelrec + 72, meshcount );
	buf_patch_i32( out, modelrec + 80, nextlocal );
	buf_patch_i32( out, modelrec + 92, nextlocal );
	buf_patch_i32( out, modelrec + 104, 0 );
	buf_patch_i32( out, modelrec + 108, 0 );

	// mesh array (contiguous)
	buf_align4( out );
	int mesh_lump = (int)out->len;
	buf_patch_i32( out, modelrec + 76, mesh_lump );
	size_t tri_patch[MAXSTUDIOMESHES];
	int wi = 0;
	for( int mii = 0; mii < NUM_MODELINFOS && wi < meshcount; mii++ )
	{
		int mio = rd_i32( mp + 36 + 4 * mii );
		if( !mio ) continue;
		int skinref = rd_i32( in + mio );
		int mc = rd_i32( in + mio + 4 ), moff = rd_i32( in + mio + 8 );
		for( int k = 0; k < mc && wi < meshcount; k++ )
		{
			const unsigned char *mesh = in + moff + (size_t)k * SZ_MESH;
			int tcount = rd_u16( mesh + 26 );
			buf_i32( out, tcount / 3 );
			tri_patch[wi] = out->len;
			buf_i32( out, 0 );
			buf_i32( out, skinref );
			buf_i32( out, mesh_norms[wi] );	// numnorms
			buf_i32( out, 0 );		// normindex (unused by the renderer)
			wi++;
		}
	}
	// tri commands (separate blob), patch mesh.triindex
	wi = 0;
	for( int mii = 0; mii < NUM_MODELINFOS && wi < meshcount; mii++ )
	{
		int mio = rd_i32( mp + 36 + 4 * mii );
		if( !mio ) continue;
		int mc = rd_i32( in + mio + 4 ), moff = rd_i32( in + mio + 8 );
		for( int k = 0; k < mc && wi < meshcount; k++ )
		{
			const unsigned char *mesh = in + moff + (size_t)k * SZ_MESH;
			int tcount = rd_u16( mesh + 26 ), tindex = rd_u16( mesh + 24 );
			buf_align4( out );
			buf_patch_i32( out, tri_patch[wi], (int)out->len );
			for( int t = 0; t + 2 < tcount; t += 3 )
			{
				int gv[3], lv[3], okm = 1;
				for( int e = 0; e < 3; e++ )
				{
					int ti = tindex + t + e;
					gv[e] = ( ti < ntris_g ) ? (int)rd_u16( in + trimap_off + (size_t)ti * 2 ) : -1;
					lv[e] = ( gv[e] >= 0 && gv[e] < nverts_g ) ? gmap[gv[e]] : -1;
					if( lv[e] < 0 ) okm = 0;
				}
				if( !okm ) continue;
				buf_u16( out, 3 );
				for( int e = 0; e < 3; e++ )
				{
					float u = 0.0f, vv = 0.0f;
					if( texco_off >= 0 && texco_off + (size_t)gv[e] * 8 + 8 <= size )
					{
						u = rd_f32( in + texco_off + (size_t)gv[e] * 8 );
						vv = rd_f32( in + texco_off + (size_t)gv[e] * 8 + 4 );
					}
					int si = (int)( u * UVSCALE + ( u < 0 ? -0.5f : 0.5f ));
					int tv = (int)( vv * UVSCALE + ( vv < 0 ? -0.5f : 0.5f ));
					if( si > 32767 ) si = 32767; if( si < -32768 ) si = -32768;
					if( tv > 32767 ) tv = 32767; if( tv < -32768 ) tv = -32768;
					buf_u16( out, (unsigned short)(short)lv[e] );
					buf_u16( out, (unsigned short)(short)lv[e] );
					buf_u16( out, (unsigned short)(short)si );
					buf_u16( out, (unsigned short)(short)tv );
				}
			}
			buf_u16( out, 0 );
			wi++;
		}
	}
	// vertex/normal bone info + arrays
	buf_align4( out );
	buf_patch_i32( out, modelrec + 84, (int)out->len );
	for( int l = 0; l < nextlocal; l++ ) buf_u8( out, (unsigned char)gbone[local2g[l]] );
	buf_align4( out );
	buf_patch_i32( out, modelrec + 88, (int)out->len );
	for( int l = 0; l < nextlocal; l++ )
		for( int k = 0; k < 3; k++ ) buf_f32( out, rd_f32( in + verts_off + (size_t)local2g[l] * 16 + k * 4 ));
	buf_patch_i32( out, modelrec + 96, (int)out->len );
	for( int l = 0; l < nextlocal; l++ ) buf_u8( out, (unsigned char)gbone[local2g[l]] );
	buf_align4( out );
	buf_patch_i32( out, modelrec + 100, (int)out->len );
	for( int l = 0; l < nextlocal; l++ )
		for( int k = 0; k < 3; k++ ) buf_f32( out, rd_f32( in + norms_off + (size_t)local2g[l] * 16 + k * 4 ));

	if( use_weights )
	{
		unsigned char w4[4]; signed char b4[4];

		buf_align4( out );
		buf_patch_i32( out, modelrec + 104, (int)out->len );
		for( int l = 0; l < nextlocal; l++ )
		{
			nf_make_boneweight( in, size, local2g[l], gbone, gbone4, scales_off, w4, b4 );
			buf_bytes( out, w4, 4 );
			buf_bytes( out, b4, 4 );
		}
		buf_align4( out );
		buf_patch_i32( out, modelrec + 108, (int)out->len );
		for( int l = 0; l < nextlocal; l++ )
		{
			nf_make_boneweight( in, size, local2g[l], gbone, gbone4, scales_off, w4, b4 );
			buf_bytes( out, w4, 4 );
			buf_bytes( out, b4, 4 );
		}
	}

	buf_align4( out );
}

byte *NFMDL_Convert14( const void *buffer, size_t size, size_t *outsize )
{
	const unsigned char *in = (const unsigned char *)buffer;
	if( !NFMDL_IsVersion14( buffer, size )) return NULL;

	const int numbones = rd_i32( in + H_BONES );
	const int numctl   = rd_i32( in + H_BONECTL );
	const int numhit   = rd_i32( in + H_HITBOX );
	const int numseq   = rd_i32( in + H_SEQ );
	const int numtex   = rd_i32( in + H_TEX );
	const int skinrefs = rd_i32( in + H_SKINREF );
	const int skinfams = rd_i32( in + H_SKINFAM );
	const int skins_off = rd_i32( in + H_SKINS );
	const int nbody    = rd_i32( in + H_BODYGRP );
	const int nattach  = rd_i32( in + H_ATTACH );
	const int nverts_g = rd_i32( in + H_VERTCOUNT );
	const int ntris_g  = rd_i32( in + H_TRICOUNT );
	const int trimap_off = rd_i32( in + H_TRIMAP );
	const int verts_off  = rd_i32( in + H_VERTS );
	const int norms_off  = rd_i32( in + H_NORMS );
	const int texco_off  = rd_i32( in + H_TEXCO );
	const int blends_off = rd_i32( in + H_BLENDING );
	const int scales_off = rd_i32( in + H_BLENDSCALE );
	const int bonefix_off = rd_i32( in + H_BONEFIX );

	if( numbones < 0 || numbones > MAXSTUDIOBONES || nverts_g < 0 || nverts_g > MAXSTUDIOVERTS ||
		ntris_g < 0 || nbody < 0 || numseq < 0 || numseq > 2048 || nbody > 4096 )
		return NULL;

	// Nightfire stores vertices in model space and provides BoneFixUp (the
	// inverse bind pose = GoldSrc "poseToBone"). We therefore skin every model
	// through the bone-weighted path with STUDIO_HAS_BONEINFO. Without a valid
	// BoneFixUp array we cannot skin correctly and fall back to rigid (which is
	// only correct if the vertices already are in bone space).
	const int has_boneinfo = numbones > 0 && bonefix_off > 0 &&
		(size_t)bonefix_off + (size_t)numbones * 48 <= size;
	const int use_weights = has_boneinfo;

	// total submodels (for the contiguous model array)
	int total_models = 0;
	for( int bg = 0; bg < nbody; bg++ )
	{
		const unsigned char *bp = in + rd_i32( in + H_BODYGRP + 4 ) + (size_t)bg * SZ_BODYGRP;
		int mc = rd_i32( bp + 64 );
		if( mc < 0 ) mc = 0;
		if( total_models + mc > 8192 ) { mc = 8192 - total_models; if( mc < 0 ) mc = 0; }
		total_models += mc;
	}

	buf_t out = { 0 };
	buf_zero( &out, 244 );	// studiohdr_t

	// ---- bones ----
	int bone_off = (int)out.len;
	for( int i = 0; i < numbones; i++ )
	{
		const unsigned char *bp = in + rd_i32( in + H_BONES + 4 ) + (size_t)i * SZ_BONE;
		char name[32]; nf_cstr( bp, 32, name, 32 );
		buf_bytes( &out, name, 32 );
		buf_i32( &out, rd_i32( bp + 32 ));
		buf_i32( &out, rd_i32( bp + 36 ));
		for( int k = 0; k < 6; k++ ) buf_i32( &out, rd_i32( bp + 40 + 4 * k ));
		for( int k = 0; k < 6; k++ ) buf_f32( &out, rd_f32( bp + 64 + 4 * k ));
		for( int k = 0; k < 6; k++ ) buf_f32( &out, rd_f32( bp + 88 + 4 * k ));
	}

	// ---- extended bone info (poseToBone), immediately after the bones array ----
	// Nightfire vertices are model-space; BoneFixUp is the inverse bind pose
	// that Xash expects as mstudioboneinfo_t.poseToBone.
	if( has_boneinfo )
	{
		for( int i = 0; i < numbones; i++ )
		{
			const unsigned char *fp = in + bonefix_off + (size_t)i * 48;
			buf_bytes( &out, fp, 48 );			// poseToBone[3][4]
			buf_zero( &out, 16 );				// qAlignment
			buf_i32( &out, 0 );				// proctype
			buf_i32( &out, 0 );				// procindex
			buf_f32( &out, 0.0f ); buf_f32( &out, 0.0f );
			buf_f32( &out, 0.0f ); buf_f32( &out, 1.0f );	// quat (identity)
			buf_zero( &out, 40 );				// reserved[10]
		}
	}

	// ---- bone controllers ----
	int ctl_off = (int)out.len;
	for( int i = 0; i < numctl; i++ )
	{
		const unsigned char *cp = in + rd_i32( in + H_BONECTL + 4 ) + (size_t)i * SZ_BONECTL;
		buf_i32( &out, rd_i32( cp ));
		buf_i32( &out, rd_i32( cp + 4 ));
		buf_f32( &out, rd_f32( cp + 8 ));
		buf_f32( &out, rd_f32( cp + 12 ));
		buf_i32( &out, rd_i32( cp + 16 ));
		buf_i32( &out, rd_i32( cp + 20 ));
	}

	// ---- hitboxes ----
	int hit_off = (int)out.len;
	for( int i = 0; i < numhit; i++ )
	{
		const unsigned char *hp = in + rd_i32( in + H_HITBOX + 4 ) + (size_t)i * SZ_HITBOX;
		buf_i32( &out, rd_i32( hp ));
		buf_i32( &out, rd_i32( hp + 4 ));
		for( int k = 0; k < 6; k++ ) buf_f32( &out, rd_f32( hp + 8 + 4 * k ));
	}

	// ---- sequence group (one inline "default") ----
	int seqgrp_off = (int)out.len;
	{
		char label[32] = "default";
		buf_bytes( &out, label, 32 );
		buf_zero( &out, 64 );
		buf_i32( &out, 0 ); buf_i32( &out, 0 );
	}

	// ---- sequence descriptions (contiguous) ----
	int seq_off = (int)out.len;
	size_t *seq_animidx_pos = (size_t *)malloc( (size_t)(numseq > 0 ? numseq : 1) * sizeof( size_t ));
	int *seq_animstart = (int *)malloc( (size_t)(numseq > 0 ? numseq : 1) * sizeof( int ));
	int *seq_blen = (int *)malloc( (size_t)(numseq > 0 ? numseq : 1) * sizeof( int ));
	if( !seq_animidx_pos || !seq_animstart || !seq_blen )
	{
		free( seq_animidx_pos ); free( seq_animstart ); free( seq_blen ); free( out.d );
		return NULL;
	}
	for( int i = 0; i < numseq; i++ )
	{
		const unsigned char *sp = in + rd_i32( in + H_SEQ + 4 ) + (size_t)i * SZ_SEQ;
		char label[32]; nf_cstr( sp, 32, label, 32 );
		int numframes = rd_i32( sp + 56 );
		int blends = rd_i32( sp + 120 );
		int animstart = rd_i32( sp + 124 );
		int blen = (int)nf_anim_block_len( in, size, animstart, numbones, blends, numframes );
		seq_animstart[i] = animstart;
		seq_blen[i] = blen;

		buf_bytes( &out, label, 32 );
		buf_f32( &out, rd_f32( sp + 32 ));
		buf_i32( &out, rd_i32( sp + 36 ));
		buf_i32( &out, rd_i32( sp + 40 ));
		buf_i32( &out, rd_i32( sp + 44 ));
		buf_i32( &out, 0 );	// numevents
		buf_i32( &out, 0 );	// eventindex
		buf_i32( &out, numframes );
		buf_i32( &out, 0 );	// weightlistindex
		buf_i32( &out, 0 );	// iklockindex
		buf_i32( &out, rd_i32( sp + 68 ));
		buf_i32( &out, rd_i32( sp + 72 ));
		for( int k = 0; k < 3; k++ ) buf_f32( &out, rd_f32( sp + 76 + 4 * k ));
		buf_i32( &out, 0 );	// autolayerindex
		buf_i32( &out, 0 );	// keyvalueindex
		for( int k = 0; k < 3; k++ ) buf_f32( &out, rd_f32( sp + 96 + 4 * k ));
		for( int k = 0; k < 3; k++ ) buf_f32( &out, rd_f32( sp + 108 + 4 * k ));
		buf_i32( &out, blen ? blends : 0 );	// numblends
		seq_animidx_pos[i] = out.len;
		buf_i32( &out, 0 );			// animindex (patched later)
		buf_i32( &out, rd_i32( sp + 128 ));
		buf_i32( &out, rd_i32( sp + 132 ));
		buf_f32( &out, rd_f32( sp + 136 ));
		buf_f32( &out, rd_f32( sp + 140 ));
		buf_f32( &out, rd_f32( sp + 144 ));
		buf_f32( &out, rd_f32( sp + 148 ));
		buf_u8( &out, 0 ); buf_u8( &out, 0 );
		buf_u8( &out, 0 ); buf_u8( &out, 0 );
		buf_i32( &out, 0 );
		buf_i32( &out, rd_i32( sp + 160 ));
		buf_i32( &out, rd_i32( sp + 164 ));
		buf_u8( &out, 0 ); buf_u8( &out, 0 ); buf_u8( &out, 0 ); buf_u8( &out, 0 );
		buf_i32( &out, 0 );
	}

	// ---- animation events ----
	// Same 76-byte layout as mstudioevent_t (frame, event, type, options[64])
	// and Half-Life event numbers (the characters use the grunt's 2-7/11 and
	// client 5001/5004), so they are copied; the converter used to drop them.
	if( numseq > 0 )
	{
		const size_t seq_size = ( out.len - (size_t)seq_off ) / (size_t)numseq;
		for( int i = 0; i < numseq; i++ )
		{
			const unsigned char *sp = in + rd_i32( in + H_SEQ + 4 ) + (size_t)i * SZ_SEQ;
			int nev = rd_i32( sp + 48 );
			int evoff = rd_i32( sp + 52 );
			if( nev <= 0 || nev > 1024 || evoff <= 0 || (size_t)evoff + (size_t)nev * SZ_EVENT > size )
				continue;
			buf_patch_i32( &out, (size_t)seq_off + i * seq_size + 48, nev );
			buf_patch_i32( &out, (size_t)seq_off + i * seq_size + 52, (int)out.len );
			for( int k = 0; k < nev; k++ )
			{
				const unsigned char *ep = in + evoff + (size_t)k * SZ_EVENT;
				char options[64] = { 0 };
				nf_cstr( ep + 12, 64, options, 64 );
				buf_i32( &out, rd_i32( ep ));
				buf_i32( &out, rd_i32( ep + 4 ));
				buf_i32( &out, rd_i32( ep + 8 ));
				buf_bytes( &out, options, 64 );
			}
		}
	}

	// ---- textures ----
	int tex_off = (int)out.len;
	for( int i = 0; i < numtex; i++ )
	{
		const unsigned char *tp = in + rd_i32( in + H_TEX + 4 ) + (size_t)i * SZ_TEX;
		char mat[64], name[64];
		nf_cstr( tp, 64, mat, 64 );
		nf_cstr( tp + 64, 64, name, 64 );
		char texname[64]; memset( texname, 0, 64 ); strncpy( texname, name, 63 );
		unsigned flags = 0;
		if( !strcmp( mat, "mdl_masked" )) flags |= STUDIO_NF_MASKED;
		else if( !strcmp( mat, "mdl_chrome" )) flags |= STUDIO_NF_CHROME;
		else if( !strcmp( mat, "mdl_additive" )) flags |= STUDIO_NF_ADDITIVE;
		else if( !strcmp( mat, "mdl_basicselfillum" ) || !strncmp( mat, "mdl_cloud", 9 ) ||
			!strncmp( mat, "mdl_sky", 7 ) || !strcmp( mat, "mdl_lightningcloud" ))
			flags |= STUDIO_NF_FULLBRIGHT;
		buf_bytes( &out, texname, 64 );
		buf_i32( &out, (int)flags );
		buf_i32( &out, UVSCALE );
		buf_i32( &out, UVSCALE );
		buf_i32( &out, 0 );
	}

	// ---- skins ----
	int skin_off = (int)out.len;
	if( skinfams > 0 && skinrefs > 0 )
		for( int f = 0; f < skinfams; f++ )
			for( int r = 0; r < skinrefs; r++ )
			{
				int o = skins_off + (f * skinrefs + r) * 2;
				buf_u16( &out, (unsigned short)(( o >= 0 && o + 2 <= (int)size ) ? rd_u16( in + o ) : 0 ));
			}

	// ---- bodyparts (contiguous) ----
	int body_off = (int)out.len;
	int base = 1;
	for( int bg = 0; bg < nbody; bg++ )
	{
		const unsigned char *bp = in + rd_i32( in + H_BODYGRP + 4 ) + (size_t)bg * SZ_BODYGRP;
		char name[64]; nf_cstr( bp, 64, name, 64 );
		int mc = rd_i32( bp + 64 );
		if( mc < 0 ) mc = 0;
		buf_bytes( &out, name, 64 );
		buf_i32( &out, mc );
		buf_i32( &out, base );
		buf_i32( &out, 0 );	// modelindex (patched)
		if( mc > 0 ) base *= mc;
		if( base <= 0 || base > 0x40000000 ) base = 1;
	}

	// ---- models array (contiguous, filled later) ----
	buf_align4( &out );
	int models_base = (int)out.len;
	buf_zero( &out, (size_t)total_models * 112 );

	int *gmap = (int *)malloc( (size_t)(nverts_g > 0 ? nverts_g : 1) * sizeof( int ));
	int *gbone = (int *)malloc( (size_t)(nverts_g > 0 ? nverts_g : 1) * sizeof( int ));
	int *gbone4 = (int *)malloc( (size_t)(nverts_g > 0 ? nverts_g : 1) * 4 * sizeof( int ));
	int *local2g = (int *)malloc( (size_t)(nverts_g > 0 ? nverts_g : 1) * sizeof( int ));
	if( !gmap || !gbone || !gbone4 || !local2g ) { free( gmap ); free( gbone ); free( gbone4 ); free( local2g ); free( out.d ); return NULL; }

	int gidx = 0;
	for( int bg = 0; bg < nbody; bg++ )
	{
		const unsigned char *bp = in + rd_i32( in + H_BODYGRP + 4 ) + (size_t)bg * SZ_BODYGRP;
		int mc = rd_i32( bp + 64 );
		if( mc < 0 ) mc = 0;
		int modeloff = rd_i32( bp + 72 );
		buf_patch_i32( &out, body_off + bg * 76 + 72, models_base + gidx * 112 );
		for( int mi = 0; mi < mc && gidx < total_models; mi++ )
		{
			size_t mo = (size_t)modeloff + (size_t)mi * SZ_MODEL;
			if( mo + SZ_MODEL > size ) break;
			emit_model_geometry( &out, in, size, models_base + (size_t)gidx * 112, mo,
				nverts_g, ntris_g, trimap_off, verts_off, norms_off, texco_off, blends_off,
				scales_off, use_weights, gmap, gbone, gbone4, local2g );
			gidx++;
		}
	}

	// ---- animation blocks (after geometry) ----
	for( int i = 0; i < numseq; i++ )
	{
		if( seq_blen[i] <= 0 ) continue;
		buf_align4( &out );
		buf_patch_i32( &out, seq_animidx_pos[i], (int)out.len );
		buf_bytes( &out, in + seq_animstart[i], (size_t)seq_blen[i] );
	}

	// ---- attachments ----
	int attach_off = (int)out.len;
	for( int i = 0; i < nattach; i++ )
	{
		const unsigned char *ap = in + rd_i32( in + H_ATTACH + 4 ) + (size_t)i * SZ_ATTACH;
		char name[32]; nf_cstr( ap, 32, name, 32 );
		buf_bytes( &out, name, 32 );
		buf_i32( &out, rd_i32( ap + 32 ));
		buf_i32( &out, rd_i32( ap + 36 ));
		for( int k = 0; k < 12; k++ ) buf_f32( &out, rd_f32( ap + 40 + 4 * k ));
	}

	// ---- header ----
	buf_patch_i32( &out, 0, IDSTUDIOHEADER );
	buf_patch_i32( &out, 4, STUDIO_VERSION );
	{
		char name[64]; memset( name, 0, 64 ); nf_cstr( in + H_NAME, 64, name, 64 );
		memcpy( out.d + 8, name, 64 );
	}
	buf_patch_i32( &out, 72, (int)out.len );
	memcpy( out.d + 76, in + H_EYE + 4, 4 );	// v10 eye.x <- nf eye.y
	memcpy( out.d + 80, in + H_EYE + 0, 4 );	// v10 eye.y <- nf eye.x
	memcpy( out.d + 84, in + H_EYE + 8, 4 );	// v10 eye.z <- nf eye.z
	memcpy( out.d + 88, in + H_MIN, 12 );
	memcpy( out.d + 100, in + H_MAX, 12 );
	memcpy( out.d + 112, in + H_BBMIN, 12 );
	memcpy( out.d + 124, in + H_BBMAX, 12 );
#define PUT(off,val) buf_patch_i32( &out, (off), (int)(val) )
	{
		unsigned hf = 0;
		if( has_boneinfo ) hf |= (1u << 30) | (1u << 31);	// BONEINFO | BONEWEIGHTS
		PUT( 136, (int)hf );
	}
	PUT( 140, numbones ); PUT( 144, bone_off );
	PUT( 148, numctl );   PUT( 152, ctl_off );
	PUT( 156, numhit );   PUT( 160, hit_off );
	PUT( 164, numseq );   PUT( 168, seq_off );
	PUT( 172, 1 );        PUT( 176, seqgrp_off );
	PUT( 180, numtex );   PUT( 184, tex_off );
	PUT( 188, (int)out.len );			// texturedataindex
	PUT( 192, skinrefs ); PUT( 196, skinfams ); PUT( 200, skin_off );
	PUT( 204, nbody );    PUT( 208, body_off );
	PUT( 212, nattach );  PUT( 216, attach_off );
	PUT( 220, 0 );
	PUT( 224, 0 ); PUT( 228, 0 ); PUT( 232, 0 );
	PUT( 236, 0 ); PUT( 240, 0 );
#undef PUT

	free( gmap ); free( gbone ); free( gbone4 ); free( local2g );
	free( seq_animidx_pos ); free( seq_animstart ); free( seq_blen );

	*outsize = out.len;
	return (byte *)out.d;
}
