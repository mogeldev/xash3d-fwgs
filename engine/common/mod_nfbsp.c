/*
mod_nfbsp.c - James Bond 007: Nightfire (PC) BSP42 -> GoldSrc BSP30 translator.

See mod_nfbsp.h and tools/nfbsp42_to_bsp30.py. This is a deliberately
self-contained translator: it depends only on libc so it can be unit-tested
outside the engine as well.

Nightfire BSP42 layout (little-endian, all offsets from file start):
    int version (=42);
    { int ofs, len; } lumps[18];
Lumps:
    0 entities, 1 planes, 2 textures(paths), 3 materials(paths), 4 vertices,
    5 zero-vertices, 6 draw-indices, 7 visibility, 8 nodes, 9 surfaces,
    10 lighting, 11 leaves, 12 leaf-surfaces, 13 leaf-brushes, 14 models,
    15 brushes, 16 brush-sides, 17 projections.

Conversion:
    - vertices are de-duplicated by position (BSP42 emits independent triangle
      lists, so coincident vertices are common) to stay under BSP30's 16-bit
      edge indices;
    - each BSP42 triangle becomes one BSP30 face (3 edges / 3 surfedges);
    - texture+lightmap projections become deduplicated BSP30 texinfo;
    - leaves' surface lists expand into BSP30 marksurfaces;
    - nodes and models map 1:1.
Lightmaps, PVS and clip hulls 1-3 are emitted empty / disabled in this pass.
*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef NFBSP_STANDALONE
// Allows compiling this translation unit on its own for unit testing without
// dragging in the whole engine (see tools/nfbsp_selftest.c).
typedef unsigned char byte;
typedef int qboolean;
#define NIGHTFIRE_BSP_VERSION 42
qboolean NFBSP_IsVersion42( const void *buffer, size_t size );
byte *NFBSP_Convert42( const void *buffer, size_t size, size_t *outsize );
#else
#include "mod_nfbsp.h"
#endif

#ifndef true
#define true 1
#define false 0
#endif

#define NF_LUMPS 18
#define BSP30_LUMPS 15
#define HLBSP_VERSION 30

static int rd_i32( const unsigned char *p )
{
	return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) |
		((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}

static float rd_f32( const unsigned char *p )
{
	int i = rd_i32( p );
	float f;
	memcpy( &f, &i, sizeof( f ));
	return f;
}

static int nf_plane_type( float nx, float ny, float nz )
{
	float ax = fabsf( nx ), ay = fabsf( ny ), az = fabsf( nz );
	int i = ( ax >= ay && ax >= az ) ? 0 : ( ay >= az ? 1 : 2 );
	if( ax + ay + az > 0.9999f && ax + ay + az < 1.0001f )
		return i;
	return i + 3;
}

static void nf_cstr( const unsigned char *p, int maxlen, char *out, int outsz )
{
	int i = 0;
	while( i < maxlen && p[i] && i < outsz - 1 )
	{
		out[i] = (char)p[i];
		i++;
	}
	out[i] = '\0';
}

// ------------------------------------------------------------------ growable buffer
typedef struct { unsigned char *d; size_t len, cap; } buf_t;

static int buf_reserve( buf_t *b, size_t extra )
{
	if( b->len + extra <= b->cap )
		return 1;
	size_t ncap = b->cap ? b->cap : 4096;
	while( ncap < b->len + extra ) ncap *= 2;
	unsigned char *nd = (unsigned char *)realloc( b->d, ncap );
	if( !nd ) return 0;
	b->d = nd; b->cap = ncap;
	return 1;
}

static void buf_bytes( buf_t *b, const void *src, size_t n )
{
	if( n == 0 ) return;	// also keeps memcpy from seeing a NULL source
	if( !buf_reserve( b, n )) return;
	memcpy( b->d + b->len, src, n );
	b->len += n;
}

static void buf_i32( buf_t *b, int v ) { buf_bytes( b, &v, 4 ); }
static void buf_f32( buf_t *b, float v ) { buf_bytes( b, &v, 4 ); }
static void buf_i16( buf_t *b, short v ) { buf_bytes( b, &v, 2 ); }
static void buf_u16( buf_t *b, unsigned short v ) { buf_bytes( b, &v, 2 ); }

static void buf_patch_i32( buf_t *b, size_t at, int v ) { memcpy( b->d + at, &v, 4 ); }

// Zero-run-length coding identical to Xash3D's Mod_CompressPVS(): a zero byte
// is followed by the count (1..255) of consecutive zero bytes; non-zero bytes
// are copied verbatim.
static void nf_rle_zero( buf_t *b, const unsigned char *src, size_t n )
{
	size_t i = 0;

	while( i < n )
	{
		unsigned char v = src[i];
		buf_bytes( b, &v, 1 );

		if( v != 0 )
		{
			i++;
			continue;
		}

		size_t j = i + 1, rep = 1;
		while( j < n && rep != 255 && src[j] == 0 )
		{
			j++;
			rep++;
		}

		unsigned char rc = (unsigned char)rep;
		buf_bytes( b, &rc, 1 );
		i = j;
	}
}

// ------------------------------------------------------------------ dedup maps
// Fixed-key open-addressing hash map used for vertex / texinfo de-duplication.
typedef struct {
	unsigned char key[40];
	int val;
	int used;
} map_slot_t;

typedef struct {
	map_slot_t *slots;
	size_t cap;   // power of two
	size_t count;
	int keysize;
} map_t;

static unsigned int fnv1a( const unsigned char *p, int n )
{
	unsigned int h = 2166136261u;
	for( int i = 0; i < n; i++ ) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static int map_init( map_t *m, int keysize, size_t capacity )
{
	size_t cap = 16;
	while( cap < capacity * 2 ) cap *= 2;
	m->slots = (map_slot_t *)calloc( cap, sizeof( map_slot_t ));
	if( !m->slots ) return 0;
	m->cap = cap; m->count = 0; m->keysize = keysize;
	return 1;
}

static void map_free( map_t *m ) { free( m->slots ); m->slots = NULL; }

// Returns 1 and sets *out if found; returns 0 if not found (and inserts with newval).
static int map_get_or_add( map_t *m, const unsigned char *key, int newval, int *out )
{
	unsigned int h = fnv1a( key, m->keysize ) & ( m->cap - 1 );
	for(;;)
	{
		map_slot_t *s = &m->slots[h];
		if( !s->used )
		{
			memcpy( s->key, key, m->keysize );
			s->val = newval;
			s->used = 1;
			m->count++;
			*out = newval;
			return 0;
		}
		if( memcmp( s->key, key, m->keysize ) == 0 )
		{
			*out = s->val;
			return 1;
		}
		h = ( h + 1 ) & ( m->cap - 1 );
	}
}

// ------------------------------------------------------------------ helpers
static int clamp_short( float f )
{
	int v = (int)lrintf( f );
	if( v < -32768 ) v = -32768;
	if( v > 32767 ) v = 32767;
	return v;
}

qboolean NFBSP_IsVersion42( const void *buffer, size_t size )
{
	if( size < 4 ) return false;
	return rd_i32( (const unsigned char *)buffer ) == NIGHTFIRE_BSP_VERSION;
}

typedef struct {
	int plane, firstedge, numedges, texinfo, lightofs;
	int surface;
	short side;
} outface_t;

typedef struct {
	int contents, visofs, firstms, numms;
	short mins[3], maxs[3];
} outleaf_t;

typedef struct {
	int planenum, c0, c1;
	short mins[3], maxs[3];
	unsigned short firstface, numfaces;
} outnode_t;

typedef struct {
	float mins[3], maxs[3], origin[3];
	int headnode[4], visleafs, firstface, numfaces;
} outmodel_t;

// ---------------------------------------------------------------------------
// Expanded world clip hulls: fragment-based solid BSP.
// NOTE: correct but not compact enough to ship yet - without a qbsp/hlcsg-style
// brush merge (interior-face removal) it over-splits badly (dm_power hull 1
// alone needs >32000 clipnodes). Disabled by default; build with
// -DNFBSP_WORLD_HULLS for experiments. See docs/world-clip-hull.md.
// ---------------------------------------------------------------------------
#ifdef NFBSP_WORLD_HULLS
#define NFH_EPS      0.05f
#define NFH_MAXV     256
#define NFH_MAXCELL  128
#define NFH_MAXDEPTH 256
#define NFH_MAXFRAG  32768
#define NFH_LIMIT    32000	// clipnodes the world hulls may consume

typedef struct { float n[3], d; int interior; int id; } nfh_plane_t;
typedef struct { nfh_plane_t *pl; int n; } nfh_frag_t;

typedef struct {
	buf_t *clip;		// lumps[9] clipnodes
	buf_t *plane;		// lumps[1] planes
	float *vs;		// scratch vertices (NFH_MAXV * 3)
	int   overflow;
} nfh_ctx_t;

static int nfh_verts( nfh_ctx_t *ctx, const nfh_plane_t *cell, int nc )
{
	int nv = 0;

	if( nc > NFH_MAXCELL ) nc = NFH_MAXCELL;
	for( int i = 0; i < nc; i++ )
	for( int j = i + 1; j < nc; j++ )
	for( int k = j + 1; k < nc; k++ )
	{
		const nfh_plane_t *a = &cell[i], *b = &cell[j], *c = &cell[k];
		float det, v[3];
		int ok = 1, dup = 0;

		det = a->n[0] * ( b->n[1] * c->n[2] - b->n[2] * c->n[1] )
		    - a->n[1] * ( b->n[0] * c->n[2] - b->n[2] * c->n[0] )
		    + a->n[2] * ( b->n[0] * c->n[1] - b->n[1] * c->n[0] );
		if( fabsf( det ) < 1e-9f ) continue;

		v[0] = ( a->d * ( b->n[1] * c->n[2] - b->n[2] * c->n[1] )
		       - a->n[1] * ( b->d * c->n[2] - b->n[2] * c->d )
		       + a->n[2] * ( b->d * c->n[1] - b->n[1] * c->d ) ) / det;
		v[1] = ( a->n[0] * ( b->d * c->n[2] - b->n[2] * c->d )
		       - a->d * ( b->n[0] * c->n[2] - b->n[2] * c->n[0] )
		       + a->n[2] * ( b->n[0] * c->d - b->d * c->n[0] ) ) / det;
		v[2] = ( a->n[0] * ( b->n[1] * c->d - b->d * c->n[1] )
		       - a->n[1] * ( b->n[0] * c->d - b->d * c->n[0] )
		       + a->d * ( b->n[0] * c->n[1] - b->n[1] * c->n[0] ) ) / det;

		for( int t = 0; t < nc; t++ )
			if( cell[t].n[0] * v[0] + cell[t].n[1] * v[1] + cell[t].n[2] * v[2] - cell[t].d > NFH_EPS )
			{ ok = 0; break; }
		if( !ok ) continue;

		for( int t = 0; t < nv; t++ )
		{
			float dx = v[0] - ctx->vs[t*3+0], dy = v[1] - ctx->vs[t*3+1], dz = v[2] - ctx->vs[t*3+2];
			if( dx*dx + dy*dy + dz*dz < 1e-4f ) { dup = 1; break; }
		}
		if( dup ) continue;

		if( nv >= NFH_MAXV ) break;
		ctx->vs[nv*3+0] = v[0]; ctx->vs[nv*3+1] = v[1]; ctx->vs[nv*3+2] = v[2];
		nv++;
	}
	return nv;
}

static int nfh_contains( const nfh_frag_t *f, const float *vs, int nv )
{
	for( int p = 0; p < f->n; p++ )
		for( int v = 0; v < nv; v++ )
			if( f->pl[p].n[0] * vs[v*3+0] + f->pl[p].n[1] * vs[v*3+1] +
			    f->pl[p].n[2] * vs[v*3+2] - f->pl[p].d > NFH_EPS )
				return 0;
	return 1;
}

static int nfh_rec( nfh_ctx_t *ctx, nfh_plane_t *cell, int nc, nfh_frag_t *frags, int nfrag, int depth )
{
	int nv, bestf = -1, bestp = -1;
	float bestscore = 1e30f;
	nfh_plane_t active[NFH_MAXCELL], P;
	int na = 0, cf, cb, idx, pidx;
	size_t slot;
	nfh_plane_t *front, *back;
	nfh_frag_t *ff, *fb;
	int nff = 0, nfb = 0;

	if( ctx->overflow || depth > NFH_MAXDEPTH ) { ctx->overflow = 1; return -1; }
	nv = nfh_verts( ctx, cell, nc );
	if( nv <= 0 ) return -1;

	for( int f = 0; f < nfrag; f++ )
		if( nfh_contains( &frags[f], ctx->vs, nv ))
			return -2;	// fully inside a brush

	for( int f = 0; f < nfrag; f++ )
		for( int p = 0; p < frags[f].n; p++ )
		{
			float nx = frags[f].pl[p].n[0], ny = frags[f].pl[p].n[1];
			float nz = frags[f].pl[p].n[2], d = frags[f].pl[p].d;
			float smin = 1e30f, smax = -1e30f, axial, imb, score;

			if( frags[f].pl[p].interior ) continue;	// inside the solid union
			for( int v = 0; v < nv; v++ )
			{
				float s = nx * ctx->vs[v*3+0] + ny * ctx->vs[v*3+1] + nz * ctx->vs[v*3+2] - d;
				if( s < smin ) smin = s;
				if( s > smax ) smax = s;
			}
			if( smin >= -NFH_EPS || smax <= NFH_EPS ) continue;
			axial = ( fabsf( fabsf( nx ) - 1.0f ) < 1e-3f ||
			          fabsf( fabsf( ny ) - 1.0f ) < 1e-3f ||
			          fabsf( fabsf( nz ) - 1.0f ) < 1e-3f ) ? 0.0f : 1000.0f;
			imb = fabsf( smin + smax ) / ( smax - smin > 1e-6f ? smax - smin : 1e-6f );
			score = axial + imb;
			if( score < bestscore ) { bestscore = score; bestf = f; bestp = p; }
		}
	if( bestf < 0 )
	{
		// No solid-union boundary plane crosses the cell: the cell is uniformly
		// inside or outside the union, so a single point test is exact.
		float c[3] = { 0.0f, 0.0f, 0.0f };
		for( int v = 0; v < nv; v++ )
		{
			c[0] += ctx->vs[v*3+0]; c[1] += ctx->vs[v*3+1]; c[2] += ctx->vs[v*3+2];
		}
		c[0] /= nv; c[1] /= nv; c[2] /= nv;
		for( int f = 0; f < nfrag; f++ )
		{
			int inside = 1;
			for( int p = 0; p < frags[f].n; p++ )
				if( frags[f].pl[p].n[0] * c[0] + frags[f].pl[p].n[1] * c[1] +
				    frags[f].pl[p].n[2] * c[2] - frags[f].pl[p].d > NFH_EPS )
				{ inside = 0; break; }
			if( inside ) return -2;
		}
		return -1;
	}

	P = frags[bestf].pl[bestp];

	// keep only the cell's active bounding planes
	for( int c = 0; c < nc && na < NFH_MAXCELL; c++ )
		for( int v = 0; v < nv; v++ )
			if( fabsf( cell[c].n[0] * ctx->vs[v*3+0] + cell[c].n[1] * ctx->vs[v*3+1] +
			    cell[c].n[2] * ctx->vs[v*3+2] - cell[c].d ) <= NFH_EPS )
			{ active[na++] = cell[c]; break; }
	if( na == 0 ) { na = nc; for( int c = 0; c < nc && c < NFH_MAXCELL; c++ ) active[c] = cell[c]; na = nc; }
	if( na > NFH_MAXCELL - 1 ) na = NFH_MAXCELL - 1;

	front = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( na + 1 ));
	back  = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( na + 1 ));
	ff = (nfh_frag_t *)malloc( sizeof( nfh_frag_t ) * ( nfrag ? nfrag : 1 ));
	fb = (nfh_frag_t *)malloc( sizeof( nfh_frag_t ) * ( nfrag ? nfrag : 1 ));
	if( !front || !back || !ff || !fb ) { ctx->overflow = 1; free( front ); free( back ); free( ff ); free( fb ); return -1; }
	for( int c = 0; c < na; c++ ) { front[c] = active[c]; back[c] = active[c]; }
	front[na].n[0] = -P.n[0]; front[na].n[1] = -P.n[1]; front[na].n[2] = -P.n[2]; front[na].d = -P.d;
	front[na].interior = 0;
	back[na] = P;

	for( int f = 0; f < nfrag; f++ )
	{
		nfh_plane_t *pf = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( frags[f].n + 1 ));
		nfh_plane_t *pb = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( frags[f].n + 1 ));

		if( !pf || !pb ) { ctx->overflow = 1; free( pf ); free( pb ); continue; }
		memcpy( pf, frags[f].pl, sizeof( nfh_plane_t ) * frags[f].n );
		pf[frags[f].n] = front[na];
		if( nfrag < NFH_MAXFRAG && nfh_verts( ctx, pf, frags[f].n + 1 ) > 0 ) { ff[nff].pl = pf; ff[nff].n = frags[f].n + 1; nff++; }
		else free( pf );

		memcpy( pb, frags[f].pl, sizeof( nfh_plane_t ) * frags[f].n );
		pb[frags[f].n] = back[na];
		if( nfrag < NFH_MAXFRAG && nfh_verts( ctx, pb, frags[f].n + 1 ) > 0 ) { fb[nfb].pl = pb; fb[nfb].n = frags[f].n + 1; nfb++; }
		else free( pb );
	}
	if( nff >= NFH_MAXFRAG || nfb >= NFH_MAXFRAG ) ctx->overflow = 1;

	// pre-order clipnode slot
	idx = (int)( ctx->clip->len / 8 );
	slot = ctx->clip->len;
	buf_i32( ctx->clip, 0 );
	buf_i16( ctx->clip, 0 );
	buf_i16( ctx->clip, 0 );

	cf = nfh_rec( ctx, front, na + 1, ff, nff, depth + 1 );
	cb = nfh_rec( ctx, back,  na + 1, fb, nfb, depth + 1 );

	for( int f = 0; f < nff; f++ ) free( ff[f].pl );
	for( int f = 0; f < nfb; f++ ) free( fb[f].pl );
	free( ff ); free( fb ); free( front ); free( back );

	if( ctx->overflow ) return -1;
	if( (int)( ctx->clip->len / 8 ) >= NFH_LIMIT ) { ctx->overflow = 1; return -1; }

	pidx = (int)( ctx->plane->len / 20 );
	buf_f32( ctx->plane, P.n[0] ); buf_f32( ctx->plane, P.n[1] );
	buf_f32( ctx->plane, P.n[2] ); buf_f32( ctx->plane, P.d );
	buf_i32( ctx->plane, nf_plane_type( P.n[0], P.n[1], P.n[2] ));

	memcpy( ctx->clip->d + slot + 0, &pidx, 4 );
	{ short s0 = (short)cf, s1 = (short)cb; memcpy( ctx->clip->d + slot + 4, &s0, 2 ); memcpy( ctx->clip->d + slot + 6, &s1, 2 ); }
	return idx;
}

// ---------------------------------------------------------------------------
// Face-based solid BSP (qbsp-style) for the world clip hulls.
// A face is a convex polygon on one expanded brush side.  Coplanar faces are
// consumed at the node whose plane they lie on; crossing faces are split.  A
// plane already used is re-used only when no fresh plane crosses the cell, so
// no boundary face ever crosses a leaf -> the union point test is exact.
// ---------------------------------------------------------------------------
#define NFW_MAXPTS 64

// Canonical (undirected) geometric plane id: +P and -P share one id so a
// geometric plane is a single candidate (qbsp surface/onnode).
static unsigned nfh_h4( float a, float b, float c, float d )
{
	unsigned h = 2166136261u, bits;
	float v[4] = { a, b, c, d };
	for( int i = 0; i < 4; i++ ) { memcpy( &bits, &v[i], 4 ); h ^= bits; h *= 16777619u; }
	return h;
}

static int nfh_geom_ids( const unsigned char *planeL, int nplanes, int *pid )
{
	int cap = 16, n = 0, i;
	float *kn;
	int *kid;

	if( nplanes <= 0 ) return 0;
	while( cap < nplanes * 2 ) cap <<= 1;
	kn = (float *)calloc( (size_t)cap * 4, sizeof( float ));
	kid = (int *)malloc( sizeof( int ) * cap );
	if( !kn || !kid ) { free( kn ); free( kid ); return 0; }
	memset( kid, -1, sizeof( int ) * cap );
	for( i = 0; i < nplanes; i++ )
	{
		float nx = rd_f32( planeL + i*20 + 0 ), ny = rd_f32( planeL + i*20 + 4 );
		float nz = rd_f32( planeL + i*20 + 8 ), d = rd_f32( planeL + i*20 + 12 );
		unsigned h;
		if( nx < 0.0f || ( nx == 0.0f && ( ny < 0.0f || ( ny == 0.0f && nz < 0.0f ))))
		{ nx = -nx; ny = -ny; nz = -nz; d = -d; }
		h = nfh_h4( nx, ny, nz, d ) & (unsigned)( cap - 1 );
		for(;;)
		{
			if( kid[h] < 0 )
			{
				kid[h] = n; kn[h*4+0] = nx; kn[h*4+1] = ny; kn[h*4+2] = nz; kn[h*4+3] = d;
				pid[i] = n++; break;
			}
			if( kn[h*4+0] == nx && kn[h*4+1] == ny && kn[h*4+2] == nz && kn[h*4+3] == d ) { pid[i] = kid[h]; break; }
			h = ( h + 1 ) & (unsigned)( cap - 1 );
		}
	}
	free( kn ); free( kid );
	return n;
}

typedef struct {
	float n[3], d;		// directed outward plane
	int   id;		// canonical geometric plane id
	int   npts;
	float *pts;		// 3*npts, convex
} nfw_face_t;

typedef struct { nfw_face_t **f; int n, cap; } nfw_list_t;

typedef struct {
	nfh_plane_t *pl;
	int n;
	float mins[3], maxs[3];
} nfw_br_t;

typedef struct {
	nfh_ctx_t    base;
	unsigned char *used;
	int          dbgmax;
	int          ncreated, nconsumed;
	int         *tried, gen;
	nfw_br_t    *br;
	int          nbr;
} nfw_ctx_t;

static void nfw_add( nfw_list_t *l, nfw_face_t *f )
{
	if( l->n >= l->cap )
	{
		int nc = l->cap ? l->cap * 2 : 16;
		nfw_face_t **nf = (nfw_face_t **)realloc( l->f, sizeof( *nf ) * nc );
		if( !nf ) return;
		l->f = nf; l->cap = nc;
	}
	l->f[l->n++] = f;
}

static nfw_face_t *nfw_newface( const float *n, float d, int id, const float *pts, int npts )
{
	nfw_face_t *f = (nfw_face_t *)malloc( sizeof( nfw_face_t ));
	if( !f ) return NULL;
	f->n[0] = n[0]; f->n[1] = n[1]; f->n[2] = n[2]; f->d = d; f->id = id; f->npts = npts;
	f->pts = (float *)malloc( sizeof( float ) * 3 * ( npts ? npts : 1 ));
	if( !f->pts ) { free( f ); return NULL; }
	memcpy( f->pts, pts, sizeof( float ) * 3 * npts );
	return f;
}

static void nfw_order( const float *n, float *pts, int npts )
{
	float c[3] = { 0, 0, 0 }, u[3], v[3], ang[NFW_MAXPTS], tmp[NFW_MAXPTS*3];
	int idx[NFW_MAXPTS], i, j;

	if( npts < 3 ) return;
	for( i = 0; i < npts; i++ ) { c[0]+=pts[i*3]; c[1]+=pts[i*3+1]; c[2]+=pts[i*3+2]; }
	c[0]/=npts; c[1]/=npts; c[2]/=npts;
	if( fabsf( n[0] ) < 0.9f ) { u[0]=1; u[1]=0; u[2]=0; } else { u[0]=0; u[1]=1; u[2]=0; }
	{
		float dot = u[0]*n[0]+u[1]*n[1]+u[2]*n[2];
		u[0]-=dot*n[0]; u[1]-=dot*n[1]; u[2]-=dot*n[2];
		{ float L=sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]); if(L>1e-9f){u[0]/=L;u[1]/=L;u[2]/=L;} }
	}
	v[0]=n[1]*u[2]-n[2]*u[1]; v[1]=n[2]*u[0]-n[0]*u[2]; v[2]=n[0]*u[1]-n[1]*u[0];
	for( i = 0; i < npts; i++ )
	{
		float dx=pts[i*3]-c[0], dy=pts[i*3+1]-c[1], dz=pts[i*3+2]-c[2];
		float a=dx*u[0]+dy*u[1]+dz*u[2], b=dx*v[0]+dy*v[1]+dz*v[2];
		ang[i]=atan2f(b,a); idx[i]=i;
	}
	for( i = 1; i < npts; i++ )
	{
		int t = idx[i]; float av = ang[t];
		for( j = i; j > 0 && ang[idx[j-1]] > av; j-- ) idx[j]=idx[j-1];
		idx[j]=t;
	}
	for( i = 0; i < npts; i++ ) { const float *p=&pts[idx[i]*3]; tmp[i*3]=p[0]; tmp[i*3+1]=p[1]; tmp[i*3+2]=p[2]; }
	memcpy( pts, tmp, sizeof( float ) * 3 * npts );
}

// split an ordered convex polygon by plane (s = n.p - d): back = s<=0, front = s>=0
static void nfw_split_poly( const float *n, float d, const float *in, int nin,
	float *back, int *nback, float *front, int *nfront )
{
	int nb = 0, nf = 0;
	for( int i = 0; i < nin; i++ )
	{
		const float *a = &in[i*3];
		const float *b = &in[((i+1)%nin)*3];
		float sa = n[0]*a[0]+n[1]*a[1]+n[2]*a[2]-d;
		float sb = n[0]*b[0]+n[1]*b[1]+n[2]*b[2]-d;
		if( sa <= NFH_EPS ) { back[nb*3]=a[0]; back[nb*3+1]=a[1]; back[nb*3+2]=a[2]; nb++; }
		if( sa >= -NFH_EPS ) { front[nf*3]=a[0]; front[nf*3+1]=a[1]; front[nf*3+2]=a[2]; nf++; }
		if(( sa > NFH_EPS && sb < -NFH_EPS ) || ( sa < -NFH_EPS && sb > NFH_EPS ))
		{
			float t = sa/(sa-sb), p[3];
			p[0]=a[0]+t*(b[0]-a[0]); p[1]=a[1]+t*(b[1]-a[1]); p[2]=a[2]+t*(b[2]-a[2]);
			back[nb*3]=p[0]; back[nb*3+1]=p[1]; back[nb*3+2]=p[2]; nb++;
			front[nf*3]=p[0]; front[nf*3+1]=p[1]; front[nf*3+2]=p[2]; nf++;
		}
	}
	*nback = nb; *nfront = nf;
}

static int nfw_point_solid( nfw_ctx_t *ctx, const float *p )
{
	for( int b = 0; b < ctx->nbr; b++ )
	{
		int ok = 1;
		for( int t = 0; t < ctx->br[b].n; t++ )
			if( ctx->br[b].pl[t].n[0]*p[0]+ctx->br[b].pl[t].n[1]*p[1]+ctx->br[b].pl[t].n[2]*p[2]-ctx->br[b].pl[t].d > NFH_EPS )
			{ ok = 0; break; }
		if( ok ) return 1;
	}
	return 0;
}

static void nfw_free_list( nfw_list_t *l )
{
	for( int i = 0; i < l->n; i++ ) { free( l->f[i]->pts ); free( l->f[i] ); }
	free( l->f ); l->f = NULL; l->n = l->cap = 0;
}

static int nfw_rec( nfw_ctx_t *ctx, nfh_plane_t *cell, int nc, nfw_list_t *faces, int depth )
{
	nfh_plane_t active[NFH_MAXCELL], P;
	nfw_list_t front = { NULL, 0, 0 }, back = { NULL, 0, 0 };
	int nv, best = -1, bestid = -1, cf, cb, idx, pidx, na = 0, bestcross = 0x7fffffff, bestax = 1;
	float cc[3] = { 0, 0, 0 }, bestdist = 1e30f;
	size_t slot;

	if( ctx->base.overflow || depth > NFH_MAXDEPTH ) { ctx->base.overflow = 1; nfw_free_list( faces ); return -1; }
	if( depth > ctx->dbgmax ) ctx->dbgmax = depth;

	nv = nfh_verts( &ctx->base, cell, nc );
	if( nv <= 0 ) { nfw_free_list( faces ); return -1; }
	for( int v = 0; v < nv; v++ )
	{ cc[0]+=ctx->base.vs[v*3]; cc[1]+=ctx->base.vs[v*3+1]; cc[2]+=ctx->base.vs[v*3+2]; }
	cc[0]/=nv; cc[1]/=nv; cc[2]/=nv;

	if( faces->n == 0 )
		return nfw_point_solid( ctx, cc ) ? -2 : -1;

	for( int i = 0; i < faces->n; i++ )
	{
		nfw_face_t *f = faces->f[i];
		int cross = 0, ax;
		float dist;

		if( f->id >= 0 && ctx->tried[f->id] == ctx->gen ) continue;	// already a candidate
		if( f->id >= 0 ) ctx->tried[f->id] = ctx->gen;

		ax = ( fabsf(fabsf(f->n[0])-1.0f) < 1e-3f ||
		       fabsf(fabsf(f->n[1])-1.0f) < 1e-3f ||
		       fabsf(fabsf(f->n[2])-1.0f) < 1e-3f ) ? 0 : 1;
		dist = fabsf( f->n[0]*cc[0]+f->n[1]*cc[1]+f->n[2]*cc[2] - f->d );

		for( int j = 0; j < faces->n; j++ )
		{
			nfw_face_t *g = faces->f[j];
			float smin = 1e30f, smax = -1e30f;
			if( g->id == f->id ) continue;
			for( int v = 0; v < g->npts; v++ )
			{
				float s = f->n[0]*g->pts[v*3]+f->n[1]*g->pts[v*3+1]+f->n[2]*g->pts[v*3+2]-f->d;
				if( s<smin ) smin=s;
				if( s>smax ) smax=s;
			}
			if( smin < -NFH_EPS && smax > NFH_EPS ) cross++;
		}
		if( cross < bestcross ||
		    ( cross == bestcross && ( ax < bestax || ( ax == bestax && dist < bestdist ))))
		{ bestcross = cross; bestax = ax; bestdist = dist; best = i; }
	}
	ctx->gen++;
	P.n[0]=faces->f[best]->n[0]; P.n[1]=faces->f[best]->n[1]; P.n[2]=faces->f[best]->n[2]; P.d=faces->f[best]->d;
	bestid = faces->f[best]->id;
	if( bestid >= 0 ) ctx->used[bestid] = 1;

	for( int i = 0; i < faces->n; i++ )
	{
		nfw_face_t *f = faces->f[i];
		float smin = 1e30f, smax = -1e30f;

		if( bestid >= 0 && f->id == bestid ) { free( f->pts ); free( f ); ctx->nconsumed++; continue; }	// consumed by this node
		for( int v = 0; v < f->npts; v++ )
		{
			float s = P.n[0]*f->pts[v*3]+P.n[1]*f->pts[v*3+1]+P.n[2]*f->pts[v*3+2]-P.d;
			if( s<smin ) smin=s;
			if( s>smax ) smax=s;
		}
		if( smin >= -NFH_EPS ) { nfw_add( &front, f ); }
		else if( smax <= NFH_EPS ) { nfw_add( &back, f ); }
		else
		{
			float *bp = (float *)malloc( sizeof(float)*3*(f->npts+2) );
			float *fp = (float *)malloc( sizeof(float)*3*(f->npts+2) );
			int nb = 0, nf = 0;
			nfw_face_t *nf1 = NULL, *nf2 = NULL;
			if( bp && fp )
			{
				nfw_split_poly( P.n, P.d, f->pts, f->npts, bp, &nb, fp, &nf );
				if( nb >= 3 ) { nfw_order( f->n, bp, nb ); nf1 = nfw_newface( f->n, f->d, f->id, bp, nb ); }
				if( nf >= 3 ) { nfw_order( f->n, fp, nf ); nf2 = nfw_newface( f->n, f->d, f->id, fp, nf ); }
			}
			free( bp ); free( fp );
			free( f->pts ); free( f );
			if( nf1 ) { nfw_add( &back, nf1 ); ctx->ncreated++; }
			if( nf2 ) { nfw_add( &front, nf2 ); ctx->ncreated++; }
		}
	}
	free( faces->f ); faces->f = NULL; faces->n = faces->cap = 0;

	for( int c = 0; c < nc && na < NFH_MAXCELL; c++ )
		for( int v = 0; v < nv; v++ )
			if( fabsf( cell[c].n[0]*ctx->base.vs[v*3]+cell[c].n[1]*ctx->base.vs[v*3+1]+
			    cell[c].n[2]*ctx->base.vs[v*3+2]-cell[c].d ) <= NFH_EPS )
			{ active[na++] = cell[c]; break; }
	if( na == 0 ) { na = nc; for( int c = 0; c < nc && c < NFH_MAXCELL; c++ ) active[c] = cell[c]; }
	if( na > NFH_MAXCELL - 1 ) na = NFH_MAXCELL - 1;

	{
		nfh_plane_t *fcell = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( na + 1 ));
		nfh_plane_t *bcell = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( na + 1 ));
		if( !fcell || !bcell ) { free( fcell ); free( bcell ); ctx->base.overflow = 1; nfw_free_list( &front ); nfw_free_list( &back ); return -1; }
		for( int c = 0; c < na; c++ ) { fcell[c] = active[c]; bcell[c] = active[c]; }
		fcell[na].n[0]=-P.n[0]; fcell[na].n[1]=-P.n[1]; fcell[na].n[2]=-P.n[2]; fcell[na].d=-P.d;
		bcell[na] = P;

		idx = (int)( ctx->base.clip->len / 8 );
		slot = ctx->base.clip->len;
		buf_i32( ctx->base.clip, 0 );
		buf_i16( ctx->base.clip, 0 );
		buf_i16( ctx->base.clip, 0 );

		cf = nfw_rec( ctx, fcell, na + 1, &front, depth + 1 );
		cb = nfw_rec( ctx, bcell, na + 1, &back, depth + 1 );
		free( fcell ); free( bcell );

		if( ctx->base.overflow ) return -1;
		if( (int)( ctx->base.clip->len / 8 ) >= NFH_LIMIT ) { ctx->base.overflow = 1; return -1; }

		pidx = (int)( ctx->base.plane->len / 20 );
		buf_f32( ctx->base.plane, P.n[0] ); buf_f32( ctx->base.plane, P.n[1] );
		buf_f32( ctx->base.plane, P.n[2] ); buf_f32( ctx->base.plane, P.d );
		buf_i32( ctx->base.plane, nf_plane_type( P.n[0], P.n[1], P.n[2] ));
		memcpy( ctx->base.clip->d + slot + 0, &pidx, 4 );
		{ short s0 = (short)cf, s1 = (short)cb; memcpy( ctx->base.clip->d + slot + 4, &s0, 2 ); memcpy( ctx->base.clip->d + slot + 6, &s1, 2 ); }
	}
	return idx;
}

// Build the world's hulls 1-3 into clips/planes. Returns 1 on success.
static int nfw_build_world( const unsigned char **L, int nleaves, int nbrushes, int nplanes,
	buf_t *clips, buf_t *planes, int headnode[4] )
{
	static const float hmins[4][3] = { {0,0,0}, {-16,-16,-36}, {-32,-32,-32}, {-16,-16,-18} };
	static const float hmaxs[4][3] = { {0,0,0}, {16,16,36}, {32,32,32}, {16,16,18} };
	int lindex = rd_i32( L[14] + 40 ), lcount = rd_i32( L[14] + 44 );
	unsigned char *seen;
	int *order, cnt = 0, ok = 1;
	nfw_ctx_t ctx;
	int *pid, ngeom;

	if( lindex < 0 || lcount <= 0 || nbrushes <= 0 ) return 0;
	seen  = (unsigned char *)calloc( nbrushes, 1 );
	order = (int *)malloc( sizeof( int ) * nbrushes );
	if( !seen || !order ) { free( seen ); free( order ); return 0; }

	for( int lk = 0; lk < lcount; lk++ )
	{
		int li = lindex + lk, lbi, lbc;
		if( li < 0 || li >= nleaves ) continue;
		lbi = rd_i32( L[11] + li * 48 + 40 );
		lbc = rd_i32( L[11] + li * 48 + 44 );
		if( lbi < 0 || lbc <= 0 ) continue;
		for( int k = 0; k < lbc; k++ )
		{
			int bi = rd_i32( L[13] + ( lbi + k ) * 4 );
			if( bi >= 0 && bi < nbrushes && !seen[bi] ) { seen[bi] = 1; order[cnt++] = bi; }
		}
	}
	if( cnt == 0 ) { free( seen ); free( order ); return 0; }
#ifdef NFBSP_STANDALONE
	fprintf( stderr, "[nfw] world brushes=%d\n", cnt );
#endif

	ctx.base.clip = clips;
	ctx.base.plane = planes;
	ctx.base.vs = (float *)malloc( sizeof( float ) * NFH_MAXV * 3 );
	ctx.base.overflow = 0;
	ctx.dbgmax = 0;
	ctx.br = (nfw_br_t *)malloc( sizeof( nfw_br_t ) * cnt );
	ctx.nbr = cnt;
	if( !ctx.base.vs || !ctx.br ) { free( ctx.base.vs ); free( ctx.br ); free( seen ); free( order ); return 0; }

	pid = (int *)malloc( sizeof( int ) * ( nplanes > 0 ? nplanes : 1 ));
	ngeom = ( pid && nplanes > 0 ) ? nfh_geom_ids( L[1], nplanes, pid ) : 0;
	ctx.used = (unsigned char *)calloc( ngeom > 0 ? ngeom : 1, 1 );
	ctx.tried = (int *)calloc( ngeom > 0 ? ngeom : 1, sizeof( int ));
	ctx.gen = 1;
	if( !pid || !ctx.used || !ctx.tried || ( nplanes > 0 && ngeom <= 0 ))
	{ free( pid ); free( ctx.used ); free( ctx.tried ); free( ctx.base.vs ); free( ctx.br ); free( seen ); free( order ); return 0; }

	// Clipnode 0 is the "use the render nodes" sentinel, so a real hull root must
	// never be 0: reserve it with a dummy clipnode when the lump is empty.
	if( clips->len == 0 ) { buf_i32( clips, 0 ); buf_i16( clips, 0 ); buf_i16( clips, 0 ); }

	for( int hull = 1; hull <= 3 && ok; hull++ )
	{
		size_t base_clip = clips->len, base_plane = planes->len;
		nfh_plane_t cell[6];
		nfw_list_t faces = { NULL, 0, 0 };
		int idx;

		memset( ctx.used, 0, (size_t)( ngeom > 0 ? ngeom : 1 ));

		// expanded brush halfspaces + bbox
		for( int k = 0; k < cnt; k++ )
		{
			int bi = order[k];
			int si = rd_i32( L[15] + bi * 12 + 4 );
			int sc = rd_i32( L[15] + bi * 12 + 8 );
			nfh_plane_t *pl = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( sc > 0 ? sc : 1 ));
			int n = 0;
			for( int j = 0; j < sc; j++ )
			{
				int pi = rd_i32( L[16] + ( si + j ) * 8 + 4 );
				float nx, ny, nz, d, sup;
				if( pi < 0 || !pl ) continue;
				nx = rd_f32( L[1] + pi * 20 + 0 );
				ny = rd_f32( L[1] + pi * 20 + 4 );
				nz = rd_f32( L[1] + pi * 20 + 8 );
				d  = rd_f32( L[1] + pi * 20 + 12 );
				sup = ( nx > 0 ? nx * hmaxs[hull][0] : nx * hmins[hull][0] )
				    + ( ny > 0 ? ny * hmaxs[hull][1] : ny * hmins[hull][1] )
				    + ( nz > 0 ? nz * hmaxs[hull][2] : nz * hmins[hull][2] );
				pl[n].n[0]=nx; pl[n].n[1]=ny; pl[n].n[2]=nz; pl[n].d=d+sup;
				pl[n].interior=0; pl[n].id = ( pi < nplanes ) ? pid[pi] : -1; n++;
			}
			ctx.br[k].pl = pl; ctx.br[k].n = n;
			{
				float *bv = (float *)malloc( sizeof(float) * NFH_MAXV * 3 );
				int nv = bv ? nfh_verts( &ctx.base, pl, n ) : 0;
				for( int t = 0; t < 3; t++ ) { ctx.br[k].mins[t] = 1e30f; ctx.br[k].maxs[t] = -1e30f; }
				for( int v = 0; v < nv; v++ )
					for( int t = 0; t < 3; t++ )
					{
						float val = ctx.base.vs[v*3+t];
						if( val < ctx.br[k].mins[t] ) ctx.br[k].mins[t] = val;
						if( val > ctx.br[k].maxs[t] ) ctx.br[k].maxs[t] = val;
					}
				free( bv );
			}
		}

		// build boundary faces + mark interior
		for( int k = 0; k < cnt && ok; k++ )
		{
			float *bv = (float *)malloc( sizeof(float) * NFH_MAXV * 3 );
			int nv = bv ? nfh_verts( &ctx.base, ctx.br[k].pl, ctx.br[k].n ) : 0;
			for( int v = 0; v < nv; v++ )
				for( int t = 0; t < 3; t++ ) bv[v*3+t] = ctx.base.vs[v*3+t];
			for( int j = 0; j < ctx.br[k].n; j++ )
			{
				float nx = ctx.br[k].pl[j].n[0], ny = ctx.br[k].pl[j].n[1], nz = ctx.br[k].pl[j].n[2], d = ctx.br[k].pl[j].d;
				float fp[NFW_MAXPTS*3], cen[3] = { 0, 0, 0 };
				int nf = 0, interior = 1, i;
				for( i = 0; i < nv && nf < NFW_MAXPTS; i++ )
					if( fabsf( nx*bv[i*3]+ny*bv[i*3+1]+nz*bv[i*3+2]-d ) <= NFH_EPS )
					{ fp[nf*3]=bv[i*3]; fp[nf*3+1]=bv[i*3+1]; fp[nf*3+2]=bv[i*3+2]; nf++; }
				if( nf < 3 ) continue;
				for( i = 0; i < nf; i++ ) { cen[0]+=fp[i*3]; cen[1]+=fp[i*3+1]; cen[2]+=fp[i*3+2]; }
				cen[0]/=nf; cen[1]/=nf; cen[2]/=nf;
				// interior iff every probe (vertices + centroid) lies inside another brush
				for( i = 0; i <= nf && interior; i++ )
				{
					float px, py, pz, q[3];
					int found = 0;
					if( i < nf ) { px=fp[i*3]; py=fp[i*3+1]; pz=fp[i*3+2]; }
					else { px=cen[0]; py=cen[1]; pz=cen[2]; }
					q[0]=px+0.2f*nx; q[1]=py+0.2f*ny; q[2]=pz+0.2f*nz;
					for( int b2 = 0; b2 < cnt && !found; b2++ )
					{
						if( b2 == k ) continue;
						if( q[0] < ctx.br[b2].mins[0]-0.1f || q[0] > ctx.br[b2].maxs[0]+0.1f ||
						    q[1] < ctx.br[b2].mins[1]-0.1f || q[1] > ctx.br[b2].maxs[1]+0.1f ||
						    q[2] < ctx.br[b2].mins[2]-0.1f || q[2] > ctx.br[b2].maxs[2]+0.1f ) continue;
						found = 1;
						for( int t = 0; t < ctx.br[b2].n; t++ )
							if( ctx.br[b2].pl[t].n[0]*q[0]+ctx.br[b2].pl[t].n[1]*q[1]+
							    ctx.br[b2].pl[t].n[2]*q[2]-ctx.br[b2].pl[t].d > NFH_EPS )
							{ found = 0; break; }
					}
					if( !found ) interior = 0;
				}
				if( interior ) continue;
				nfw_order( ctx.br[k].pl[j].n, fp, nf );
				{
					nfw_face_t *fc = nfw_newface( ctx.br[k].pl[j].n, d, ctx.br[k].pl[j].id, fp, nf );
					if( fc ) nfw_add( &faces, fc );
				}
			}
			free( bv );
		}

		cell[0].n[0] = -1; cell[0].n[1] = 0; cell[0].n[2] = 0; cell[0].d = -rd_f32( L[14] + 0 );
		cell[1].n[0] =  1; cell[1].n[1] = 0; cell[1].n[2] = 0; cell[1].d =  rd_f32( L[14] + 12 );
		cell[2].n[0] = 0; cell[2].n[1] = -1; cell[2].n[2] = 0; cell[2].d = -rd_f32( L[14] + 4 );
		cell[3].n[0] = 0; cell[3].n[1] =  1; cell[3].n[2] = 0; cell[3].d =  rd_f32( L[14] + 16 );
		cell[4].n[0] = 0; cell[4].n[1] = 0; cell[4].n[2] = -1; cell[4].d = -rd_f32( L[14] + 8 );
		cell[5].n[0] = 0; cell[5].n[1] = 0; cell[5].n[2] =  1; cell[5].d =  rd_f32( L[14] + 20 );

		ctx.base.overflow = 0;
		ctx.dbgmax = 0;
		ctx.ncreated = 0; ctx.nconsumed = 0;
		ctx.gen = 1;
#ifdef NFBSP_STANDALONE
		fprintf( stderr, "[nfw] hull %d: boundary faces=%d\n", hull, faces.n );
#endif
		idx = nfw_rec( &ctx, cell, 6, &faces, 0 );
#ifdef NFBSP_STANDALONE
		fprintf( stderr, "[nfw] hull %d: idx=%d overflow=%d clips=%zu planes=%zu depth=%d created=%d consumed=%d\n",
			hull, idx, ctx.base.overflow, clips->len / 8, planes->len / 20, ctx.dbgmax, ctx.ncreated, ctx.nconsumed );
#endif
		nfw_free_list( &faces );
		for( int k = 0; k < cnt; k++ ) free( ctx.br[k].pl );

		if( ctx.base.overflow || idx < 0 )
		{
			clips->len = base_clip; planes->len = base_plane; ok = 0; break;
		}
		headnode[hull] = idx;
	}

	free( pid ); free( ctx.used ); free( ctx.tried ); free( ctx.br ); free( ctx.base.vs );
	free( seen ); free( order );
	return ok;
}

// Build the world's hulls 1-3 into clips/planes. Returns 1 on success.
static int nfh_build_world( const unsigned char **L, int nleaves, int nbrushes,
	buf_t *clips, buf_t *planes, int headnode[4] )
{
	static const float hmins[4][3] = { {0,0,0}, {-16,-16,-36}, {-32,-32,-32}, {-16,-16,-18} };
	static const float hmaxs[4][3] = { {0,0,0}, {16,16,36}, {32,32,32}, {16,16,18} };
	int lindex = rd_i32( L[14] + 40 ), lcount = rd_i32( L[14] + 44 );
	unsigned char *seen;
	int *order, cnt = 0, ok = 1;
	nfh_ctx_t ctx;

	if( lindex < 0 || lcount <= 0 || nbrushes <= 0 ) return 0;
	seen  = (unsigned char *)calloc( nbrushes, 1 );
	order = (int *)malloc( sizeof( int ) * nbrushes );
	if( !seen || !order ) { free( seen ); free( order ); return 0; }

	for( int lk = 0; lk < lcount; lk++ )
	{
		int li = lindex + lk, lbi, lbc;
		if( li < 0 || li >= nleaves ) continue;
		lbi = rd_i32( L[11] + li * 48 + 40 );
		lbc = rd_i32( L[11] + li * 48 + 44 );
		if( lbi < 0 || lbc <= 0 ) continue;
		for( int k = 0; k < lbc; k++ )
		{
			int bi = rd_i32( L[13] + ( lbi + k ) * 4 );
			if( bi >= 0 && bi < nbrushes && !seen[bi] ) { seen[bi] = 1; order[cnt++] = bi; }
		}
	}
	if( cnt == 0 ) { free( seen ); free( order ); return 0; }
#ifdef NFBSP_STANDALONE
	fprintf( stderr, "[nfh] world brushes=%d\n", cnt );
#endif

	ctx.clip = clips;
	ctx.plane = planes;
	ctx.vs = (float *)malloc( sizeof( float ) * NFH_MAXV * 3 );
	if( !ctx.vs ) { free( seen ); free( order ); return 0; }

	for( int hull = 1; hull <= 3 && ok; hull++ )
	{
		size_t base_clip = clips->len, base_plane = planes->len;
		nfh_frag_t *frags = (nfh_frag_t *)malloc( sizeof( nfh_frag_t ) * cnt );
		nfh_plane_t cell[6];
		int idx;

		if( !frags ) { ok = 0; break; }
		for( int k = 0; k < cnt; k++ )
		{
			int bi = order[k];
			int si = rd_i32( L[15] + bi * 12 + 4 );
			int sc = rd_i32( L[15] + bi * 12 + 8 );
			nfh_plane_t *pl = (nfh_plane_t *)malloc( sizeof( nfh_plane_t ) * ( sc > 0 ? sc : 1 ));
			int n = 0;
			for( int j = 0; j < sc; j++ )
			{
				int pi = rd_i32( L[16] + ( si + j ) * 8 + 4 );
				float nx, ny, nz, d, sup;
				if( pi < 0 ) continue;
				nx = rd_f32( L[1] + pi * 20 + 0 );
				ny = rd_f32( L[1] + pi * 20 + 4 );
				nz = rd_f32( L[1] + pi * 20 + 8 );
				d  = rd_f32( L[1] + pi * 20 + 12 );
				sup = ( nx > 0 ? nx * hmaxs[hull][0] : nx * hmins[hull][0] )
				    + ( ny > 0 ? ny * hmaxs[hull][1] : ny * hmins[hull][1] )
				    + ( nz > 0 ? nz * hmaxs[hull][2] : nz * hmins[hull][2] );
				pl[n].n[0] = nx; pl[n].n[1] = ny; pl[n].n[2] = nz; pl[n].d = d + sup; pl[n].interior = 0; n++;
			}
			frags[k].pl = pl; frags[k].n = n;
		}

		// ---- mark interior faces (covered by another brush) ----
		// A face is interior iff every probe (face vertices + centroid) offset
		// slightly along the outward normal lies inside another brush. This is
		// conservative: partial coverage keeps the face as a boundary.
		{
			int *fvc = (int *)malloc( sizeof( int ) * cnt );
			float **fv = (float **)malloc( sizeof( float * ) * cnt );
			float *flo = (float *)malloc( sizeof( float ) * cnt * 3 );
			float *fhi = (float *)malloc( sizeof( float ) * cnt * 3 );

			if( fvc && fv && flo && fhi )
			{
				for( int k = 0; k < cnt; k++ )
				{
					int nvk = nfh_verts( &ctx, frags[k].pl, frags[k].n );
					int i;
					fvc[k] = nvk > 0 ? nvk : 0;
					fv[k] = (float *)malloc( sizeof( float ) * 3 * ( fvc[k] ? fvc[k] : 1 ));
					for( i = 0; i < fvc[k]; i++ )
					{
						fv[k][i*3+0] = ctx.vs[i*3+0]; fv[k][i*3+1] = ctx.vs[i*3+1]; fv[k][i*3+2] = ctx.vs[i*3+2];
					}
					flo[k*3+0] = flo[k*3+1] = flo[k*3+2] = 1e30f;
					fhi[k*3+0] = fhi[k*3+1] = fhi[k*3+2] = -1e30f;
					for( i = 0; i < fvc[k]; i++ )
						for( int t = 0; t < 3; t++ )
						{
							float val = fv[k][i*3+t];
							if( val < flo[k*3+t] ) flo[k*3+t] = val;
							if( val > fhi[k*3+t] ) fhi[k*3+t] = val;
						}
				}

				for( int k = 0; k < cnt; k++ )
				{
					for( int p = 0; p < frags[k].n; p++ )
					{
						float nx = frags[k].pl[p].n[0], ny = frags[k].pl[p].n[1], nz = frags[k].pl[p].n[2], d = frags[k].pl[p].d;
						float probes[40][3];
						int np = 0, interior = 1;
						for( int i = 0; i < fvc[k] && np < 32; i++ )
							if( fabsf( nx * fv[k][i*3+0] + ny * fv[k][i*3+1] + nz * fv[k][i*3+2] - d ) <= NFH_EPS )
							{
								probes[np][0] = fv[k][i*3+0]; probes[np][1] = fv[k][i*3+1]; probes[np][2] = fv[k][i*3+2]; np++;
							}
						if( np < 3 ) continue;	// degenerate face
						{	// centroid probe
							float c[3] = { 0,0,0 };
							for( int i = 0; i < np; i++ ) { c[0]+=probes[i][0]; c[1]+=probes[i][1]; c[2]+=probes[i][2]; }
							c[0]/=np; c[1]/=np; c[2]/=np;
							probes[np][0]=c[0]; probes[np][1]=c[1]; probes[np][2]=c[2]; np++;
						}
						for( int i = 0; i < np && interior; i++ )
						{
							float q[3] = { probes[i][0] + 0.2f*nx, probes[i][1] + 0.2f*ny, probes[i][2] + 0.2f*nz };
							int found = 0;
							for( int j = 0; j < cnt && !found; j++ )
							{
								if( j == k ) continue;
								if( q[0] < flo[j*3+0]-0.1f || q[0] > fhi[j*3+0]+0.1f ||
								    q[1] < flo[j*3+1]-0.1f || q[1] > fhi[j*3+1]+0.1f ||
								    q[2] < flo[j*3+2]-0.1f || q[2] > fhi[j*3+2]+0.1f ) continue;
								found = 1;
								for( int t = 0; t < frags[j].n; t++ )
									if( frags[j].pl[t].n[0]*q[0] + frags[j].pl[t].n[1]*q[1] +
									    frags[j].pl[t].n[2]*q[2] - frags[j].pl[t].d > NFH_EPS )
									{ found = 0; break; }
							}
							if( !found ) interior = 0;
						}
						frags[k].pl[p].interior = interior;
					}
				}
				for( int k = 0; k < cnt; k++ ) free( fv[k] );
			}
			free( fvc ); free( fv ); free( flo ); free( fhi );
		}
#ifdef NFBSP_STANDALONE
		{
			int ti = 0, tt = 0;
			for( int k = 0; k < cnt; k++ )
				for( int p = 0; p < frags[k].n; p++ ) { tt++; if( frags[k].pl[p].interior ) ti++; }
			fprintf( stderr, "[nfh] hull %d: interior faces %d / %d\n", hull, ti, tt );
		}
#endif

		cell[0].n[0] = -1; cell[0].n[1] = 0; cell[0].n[2] = 0; cell[0].d = -rd_f32( L[14] + 0 );
		cell[1].n[0] =  1; cell[1].n[1] = 0; cell[1].n[2] = 0; cell[1].d =  rd_f32( L[14] + 12 );
		cell[2].n[0] = 0; cell[2].n[1] = -1; cell[2].n[2] = 0; cell[2].d = -rd_f32( L[14] + 4 );
		cell[3].n[0] = 0; cell[3].n[1] =  1; cell[3].n[2] = 0; cell[3].d =  rd_f32( L[14] + 16 );
		cell[4].n[0] = 0; cell[4].n[1] = 0; cell[4].n[2] = -1; cell[4].d = -rd_f32( L[14] + 8 );
		cell[5].n[0] = 0; cell[5].n[1] = 0; cell[5].n[2] =  1; cell[5].d =  rd_f32( L[14] + 20 );

		ctx.overflow = 0;
		idx = nfh_rec( &ctx, cell, 6, frags, cnt, 0 );
#ifdef NFBSP_STANDALONE
		fprintf( stderr, "[nfh] hull %d: idx=%d overflow=%d clips=%zu planes=%zu\n",
			hull, idx, ctx.overflow, clips->len / 8, planes->len / 20 );
#endif

		for( int k = 0; k < cnt; k++ ) free( frags[k].pl );
		free( frags );

		if( ctx.overflow || idx < 0 )
		{
			clips->len = base_clip;
			planes->len = base_plane;
			ok = 0;
			break;
		}
		headnode[hull] = idx;
	}

	free( ctx.vs );
	free( seen ); free( order );
	return ok;
}
#endif // NFBSP_WORLD_HULLS

byte *NFBSP_Convert42( const void *buffer, size_t size, size_t *outsize )
{
	const unsigned char *in = (const unsigned char *)buffer;

	if( size < 4 + NF_LUMPS * 8 || rd_i32( in ) != NIGHTFIRE_BSP_VERSION )
		return NULL;

	int lofs[NF_LUMPS], llen[NF_LUMPS];
	for( int i = 0; i < NF_LUMPS; i++ )
	{
		lofs[i] = rd_i32( in + 4 + i * 8 );
		llen[i] = rd_i32( in + 8 + i * 8 );
		if( lofs[i] < 0 || llen[i] < 0 || (size_t)lofs[i] + (size_t)llen[i] > size )
			return NULL;
	}

	const unsigned char *L[NF_LUMPS];
	for( int i = 0; i < NF_LUMPS; i++ ) L[i] = in + lofs[i];

	int nplanes    = llen[1] / 20;
	int nverts     = llen[4] / 12;
	int nnodes     = llen[8] / 36;
	int nsurfaces  = llen[9] / 48;
	int nleaves    = llen[11] / 48;
	int nmodels    = llen[14] / 56;
	int ntextures  = llen[2] / 64;

	// World leaves are 0 .. (leavesIndex+leavesCount-1); Nightfire appends
	// brush-model leaves after them. Xash3D assumes world numleafs ==
	// submodels[0].visleafs, and the BSP nodes never reference the brush leaves,
	// so we emit only the world leaves.
	int world_leaf_index = 0, world_leaf_count = 0, nleaves_out;
	if( nmodels > 0 )
	{
		world_leaf_index = rd_i32( L[14] + 40 );
		world_leaf_count = rd_i32( L[14] + 44 );
	}
	nleaves_out = world_leaf_index + world_leaf_count;
	if( nleaves_out > nleaves || nleaves_out <= 0 )
		nleaves_out = nleaves;

	// ---- output dynamic arrays
	float  *ov = NULL; size_t nov = 0; size_t cov = 0;          // vertices (x,y,z)
	unsigned short *oe0 = NULL, *oe1 = NULL; size_t noe = 0;    // edges
	int    *ose = NULL; size_t nose = 0; size_t cose = 0;       // surfedges
	outface_t *of = NULL; size_t nof = 0; size_t cof = 0;       // faces
	unsigned short *oms = NULL; size_t nms = 0; // marksurfaces
	outleaf_t *ol = NULL;                                       // leaves
	outnode_t *on = NULL;                                       // nodes
	outmodel_t *om = NULL;                                      // models

	// surface -> first face index / face count
	int *surf_first = (int *)malloc( sizeof( int ) * ( nsurfaces ? nsurfaces : 1 ));
	int *surf_count = (int *)calloc( nsurfaces ? nsurfaces : 1, sizeof( int ));

	// texinfo (deduplicated)
	typedef struct { float s[4], t[4]; int miptex, flags; } texinfo_t;
	texinfo_t *oti = NULL; size_t noti = 0; size_t coti = 0;
	map_t ti_map;

	map_t vmap;
	if( !map_init( &vmap, 12, (size_t)nverts + 16 ) ||
		!map_init( &ti_map, 40, (size_t)nsurfaces + 16 ))
	{
		*outsize = 0; return NULL;
	}

	int ok = 1;

	// local macro-ish helpers (manual, to keep control flow explicit)
	#define PUSH_VERT(x,y,z) do { \
		if( nov + 3 > cov ) { cov = cov ? cov * 2 : 8192; ov = (float *)realloc( ov, cov * sizeof(float) ); } \
		ov[nov++] = (x); ov[nov++] = (y); ov[nov++] = (z); } while(0)
	#define PUSH_EDGE(a,b) do { \
		if( noe + 1 > n_edge_cap ) { n_edge_cap = n_edge_cap ? n_edge_cap * 2 : 16384; oe0 = (unsigned short *)realloc( oe0, n_edge_cap * sizeof(unsigned short) ); oe1 = (unsigned short *)realloc( oe1, n_edge_cap * sizeof(unsigned short) ); } \
		oe0[noe] = (unsigned short)(a); oe1[noe] = (unsigned short)(b); noe++; } while(0)
	#define PUSH_SURFEDGE(e) do { \
		if( nose + 1 > cose ) { cose = cose ? cose * 2 : 16384; ose = (int *)realloc( ose, cose * sizeof(int) ); } \
		ose[nose++] = (e); } while(0)
	#define PUSH_FACE(s) do { \
		if( nof + 1 > cof ) { cof = cof ? cof * 2 : 8192; of = (outface_t *)realloc( of, cof * sizeof(outface_t) ); } \
		of[nof++] = (s); } while(0)

	size_t n_edge_cap = 0;
	int *leaf_visofs = NULL;
	buf_t vislump = { 0 };

	// ---- material pass
	// Nightfire associates a material (wld_masked, wld_glass, wld_fullbright,
	// ...) with every surface. GoldSrc conveys transparency through a '{' texture
	// name prefix (which the engine turns into SURF_TRANSPARENT), so mark any
	// texture used by a masked/glass surface. Surfaces textured with the
	// invisible "special/*" helpers are dropped entirely.
	unsigned char *tex_masked = (unsigned char *)calloc( ntextures ? ntextures : 1, 1 );
	unsigned char *surf_skip = (unsigned char *)calloc( nsurfaces ? nsurfaces : 1, 1 );
	const int nmat = llen[3] / 64;

	if( !tex_masked || !surf_skip )
	{
		free( tex_masked ); free( surf_skip );
		free( ov ); free( oe0 ); free( oe1 ); free( ose ); free( of );
		free( oms ); free( oti ); free( surf_first ); free( surf_count );
		map_free( &vmap ); map_free( &ti_map );
		*outsize = 0; return NULL;
	}

	for( int si = 0; si < nsurfaces; si++ )
	{
		const unsigned char *sp = L[9] + si * 48;
		int texidx = rd_i32( sp + 24 );
		int matidx = rd_i32( sp + 28 );
		char mat[64], texname[64];

		if( texidx >= 0 && texidx < ntextures )
		{
			int is_masked = 0;
			const char *base;

			nf_cstr( L[2] + texidx * 64, 64, texname, sizeof( texname ));

			if( matidx >= 0 && matidx < nmat )
			{
				nf_cstr( L[3] + matidx * 64, 64, mat, sizeof( mat ));
				if( strstr( mat, "masked" ) || strstr( mat, "glass" ))
					is_masked = 1;
			}

			// Nightfire also marks masked textures with a '{' before the file
			// name, e.g. "airfield/{steel_ladder02"
			base = strrchr( texname, '/' );
			base = base ? base + 1 : texname;
			if( base[0] == '{' )
				is_masked = 1;

			if( is_masked )
				tex_masked[texidx] = 1;

			if( !strcmp( texname, "special/nodraw" ) || !strcmp( texname, "special/bevel" ))
				surf_skip[si] = 1;
		}
	}

	int poly[256];
	for( int si = 0; si < nsurfaces && ok; si++ )
	{
		const unsigned char *sp = L[9] + si * 48;
		int plane = rd_i32( sp + 0 );
		int vindex = rd_i32( sp + 4 );
		int vcount = rd_i32( sp + 8 );
		int tex    = rd_i32( sp + 24 );
		int texproj= rd_i32( sp + 32 );
		int npoly = 0;

		surf_first[si] = (int)nof;
		surf_count[si] = 0;

		if( vindex + vcount > nverts || plane >= nplanes )
		{
			ok = 0; break;
		}
		if( surf_skip[si] || vcount < 3 )
			continue;

		// texinfo (texture projection)
		const unsigned char *pp = L[17] + texproj * 32;
		texinfo_t ti;
		for( int k = 0; k < 4; k++ ) ti.s[k] = rd_f32( pp + k * 4 );
		for( int k = 0; k < 4; k++ ) ti.t[k] = rd_f32( pp + 16 + k * 4 );
		ti.miptex = tex; ti.flags = 0;

		unsigned char tikey[40];
		int ti_index;
		memcpy( tikey, &ti, 40 );
		if( !map_get_or_add( &ti_map, tikey, (int)noti, &ti_index ) )
		{
			if( noti + 1 > coti ) { coti = coti ? coti * 2 : 4096; oti = (texinfo_t *)realloc( oti, coti * sizeof(texinfo_t) ); }
			oti[noti++] = ti;
			ti_index = (int)noti - 1;
		}

		// The Nightfire surface's vertex block is an ordered convex polygon
		// (verified 16090/16095 surfaces), so emit one BSP30 face per surface.
		for( int k = 0; k < vcount && npoly < 256; k++ )
		{
			float x = rd_f32( L[4] + ( vindex + k ) * 12 + 0 );
			float y = rd_f32( L[4] + ( vindex + k ) * 12 + 4 );
			float z = rd_f32( L[4] + ( vindex + k ) * 12 + 8 );
			int qx = (int)lrintf( x * 100.0f );
			int qy = (int)lrintf( y * 100.0f );
			int qz = (int)lrintf( z * 100.0f );
			unsigned char vkey[12];
			int idx;

			memcpy( vkey + 0, &qx, 4 );
			memcpy( vkey + 4, &qy, 4 );
			memcpy( vkey + 8, &qz, 4 );
			if( !map_get_or_add( &vmap, vkey, (int)( nov / 3 ), &idx ))
			{
				PUSH_VERT( x, y, z );
				idx = (int)( nov / 3 ) - 1;
			}
			if( npoly == 0 || poly[npoly-1] != idx )
				poly[npoly++] = idx;
		}
		if( npoly > 2 && poly[0] == poly[npoly-1] )
			npoly--;
		if( npoly < 3 )
			continue;

		// Newell normal -> face side vs the stored plane
		{
			float nx = 0, ny = 0, nz = 0;
			const unsigned char *plp = L[1] + plane * 20;
			float pnx = rd_f32( plp ), pny = rd_f32( plp + 4 ), pnz = rd_f32( plp + 8 );
			short side;
			int firstedge;

			for( int k = 0; k < npoly; k++ )
			{
				float *a = ov + poly[k] * 3;
				float *b = ov + poly[(k + 1) % npoly] * 3;
				nx += ( a[1] - b[1] ) * ( a[2] + b[2] );
				ny += ( a[2] - b[2] ) * ( a[0] + b[0] );
				nz += ( a[0] - b[0] ) * ( a[1] + b[1] );
			}
			side = ( nx * pnx + ny * pny + nz * pnz ) >= 0 ? 0 : 1;

			firstedge = (int)nose;
			for( int k = 0; k < npoly; k++ )
			{
				PUSH_SURFEDGE( (int)noe );
				PUSH_EDGE( poly[k], poly[(k + 1) % npoly] );
			}

			outface_t f;
			f.plane = plane; f.side = side; f.firstedge = firstedge;
			f.numedges = npoly; f.texinfo = ti_index; f.lightofs = -1;
			f.surface = si;
			PUSH_FACE( f );
			surf_count[si] = 1;
		}
	}

	if( !ok )
	{
		free( ov ); free( oe0 ); free( oe1 ); free( ose ); free( of );
		free( oms ); free( oti ); free( surf_first ); free( surf_count );
		free( tex_masked ); free( surf_skip ); free( leaf_visofs ); free( vislump.d );
		map_free( &vmap ); map_free( &ti_map );
		*outsize = 0; return NULL;
	}

	// ------------------------------------------------------------ lightmaps
	// Resample each Nightfire per-surface lightmap onto GoldSrc's per-face grid.
	// GoldSrc derives its grid from the *texture* projection (texel/16), while
	// Nightfire's lightmap grid uses the *lightmap* projection, so we project
	// every GoldSrc luxel back to 3D and sample the Nightfire lightmap.
	buf_t lighting = { 0 };
	{
		const size_t lm_budget = 24u * 1024u * 1024u;
		float us[256], ts[256], usn[256], tsn[256];

		for( size_t fi = 0; fi < nof; fi++ )
		{
			outface_t *f = &of[fi];
			const unsigned char *sp = L[9] + f->surface * 48;
			int vindex = rd_i32( sp + 4 );
			int vcount = rd_i32( sp + 8 );
			int lproj  = rd_i32( sp + 36 );
			int litofs = rd_i32( sp + 44 );
			texinfo_t *txi = &oti[f->texinfo];
			const unsigned char *lpp;
			float S[4], T[4];
			float bmin0, bmax0, bmin1, bmax1, wmin, hmin;
			int w, h, wn, hn;

			if( litofs < 0 || lighting.len >= lm_budget || vcount < 3 || vcount > 256 )
				continue;

			// GoldSrc grid from the texinfo (texture) projection
			for( int k = 0; k < f->numedges; k++ )
			{
				int e = ose[f->firstedge + k];
				float *p = ov + oe0[e >= 0 ? e : -e] * 3;
				us[k] = txi->s[0]*p[0] + txi->s[1]*p[1] + txi->s[2]*p[2] + txi->s[3];
				ts[k] = txi->t[0]*p[0] + txi->t[1]*p[1] + txi->t[2]*p[2] + txi->t[3];
			}
			bmin0 = bmax0 = us[0]; bmin1 = bmax1 = ts[0];
			for( int k = 1; k < f->numedges; k++ )
			{
				if( us[k] < bmin0 ) bmin0 = us[k];
				if( us[k] > bmax0 ) bmax0 = us[k];
				if( ts[k] < bmin1 ) bmin1 = ts[k];
				if( ts[k] > bmax1 ) bmax1 = ts[k];
			}
			{
				int b0 = (int)floorf( bmin0 / 16.0f ), b0m = (int)ceilf( bmax0 / 16.0f );
				int b1 = (int)floorf( bmin1 / 16.0f ), b1m = (int)ceilf( bmax1 / 16.0f );
				w = b0m - b0 + 1; h = b1m - b1 + 1;
				bmin0 = (float)b0; bmin1 = (float)b1;
			}
			if( w < 1 || h < 1 || w > 1024 || h > 1024 )
				continue;

			lpp = L[17] + lproj * 32;
			for( int k = 0; k < 4; k++ ) { S[k] = rd_f32( lpp + k * 4 ); T[k] = rd_f32( lpp + 16 + k * 4 ); }
			for( int k = 0; k < vcount; k++ )
			{
				float x = rd_f32( L[4] + ( vindex + k ) * 12 + 0 );
				float y = rd_f32( L[4] + ( vindex + k ) * 12 + 4 );
				float z = rd_f32( L[4] + ( vindex + k ) * 12 + 8 );
				usn[k] = S[0]*x + S[1]*y + S[2]*z + S[3];
				tsn[k] = T[0]*x + T[1]*y + T[2]*z + T[3];
			}
			{
				float mn0 = usn[0], mx0 = usn[0], mn1 = tsn[0], mx1 = tsn[0];
				for( int k = 1; k < vcount; k++ )
				{
					if( usn[k] < mn0 ) mn0 = usn[k];
					if( usn[k] > mx0 ) mx0 = usn[k];
					if( tsn[k] < mn1 ) mn1 = tsn[k];
					if( tsn[k] > mx1 ) mx1 = tsn[k];
				}
				wmin = floorf( mn0 ); hmin = floorf( mn1 );
				wn = (int)ceilf( mx0 ) - (int)wmin + 1;
				hn = (int)ceilf( mx1 ) - (int)hmin + 1;
				if( wn < 1 || hn < 1 ) continue;
				if( (size_t)litofs + (size_t)3 * wn * hn > (size_t)llen[10] ) continue;

				{
					const unsigned char *block = L[10] + litofs;
					const unsigned char *plp = L[1] + f->plane * 20;
					float pnx = rd_f32( plp ), pny = rd_f32( plp + 4 );
					float pnz = rd_f32( plp + 8 ), pd = rd_f32( plp + 12 );
					float tx0 = bmin0 * 16.0f, tx1 = bmin1 * 16.0f;
					int i, j;

					f->lightofs = (int)lighting.len;
					for( j = 0; j < h; j++ )
					for( i = 0; i < w; i++ )
					{
						float ax=txi->s[0], ay=txi->s[1], az=txi->s[2], ar=tx0 + i*16.0f - txi->s[3];
						float bx=txi->t[0], by=txi->t[1], bz=txi->t[2], br=tx1 + j*16.0f - txi->t[3];
						float det = ax*(by*pnz-bz*pny) - ay*(bx*pnz-bz*pnx) + az*(bx*pny-by*pnx);
						float px, py, pz, un, tn;
						int ii, jj;

						if( fabsf( det ) < 1e-9f )
						{
							unsigned char z3[3] = { 0, 0, 0 };
							buf_bytes( &lighting, z3, 3 );
							continue;
						}
						px = (ar*(by*pnz-bz*pny) - ay*(br*pnz-bz*pd) + az*(br*pny-by*pd)) / det;
						py = (ax*(br*pnz-bz*pd) - ar*(bx*pnz-bz*pnx) + az*(bx*pd-br*pnx)) / det;
						pz = (ax*(by*pd-br*pny) - ay*(bx*pd-br*pnx) + ar*(bx*pny-by*pnx)) / det;
						un = S[0]*px + S[1]*py + S[2]*pz + S[3];
						tn = T[0]*px + T[1]*py + T[2]*pz + T[3];
						ii = (int)floorf( un ) - (int)wmin;
						jj = (int)floorf( tn ) - (int)hmin;
						if( ii < 0 ) ii = 0; if( ii >= wn ) ii = wn - 1;
						if( jj < 0 ) jj = 0; if( jj >= hn ) jj = hn - 1;
						buf_bytes( &lighting, block + ( jj * wn + ii ) * 3, 3 );
					}
				}
			}
		}
	}

	// ---- visibility: Nightfire fixed-size non-RLE PVS -> GoldSrc RLE PVS.
	// Nightfire stores one fixed-size record per world leaf (record for leaf i
	// at leaves[i].visibilityOffset); bit n means "can see leaf n+1". GoldSrc
	// indexes clusters as (leaf - 1), so the uncompressed cluster bit vector is
	// simply the record's first visbytes bytes, RLE-encoded.
	leaf_visofs = (int *)malloc( sizeof( int ) * ( nleaves ? nleaves : 1 ));
	for( int i = 0; i < nleaves; i++ )
		leaf_visofs[i] = -1;

	{
		int rec_size = 0;
		const size_t visbytes = ((size_t)nleaves_out + 7) / 8;

		for( int i = 0; i < nleaves; i++ )
		{
			int off = rd_i32( L[11] + i * 48 + 4 );
			if( off > 0 && ( rec_size == 0 || off < rec_size ))
				rec_size = off;
		}

		if( rec_size > 0 && visbytes > 0 )
		{
			for( int k = 0; k < world_leaf_count; k++ )
			{
				int i = world_leaf_index + k;
				int off;

				if( i < 0 || i >= nleaves )
					continue;

				off = rd_i32( L[11] + i * 48 + 4 );
				if( off < 0 || (size_t)off + (size_t)rec_size > (size_t)llen[7] )
					continue;

				leaf_visofs[i] = (int)vislump.len;
				nf_rle_zero( &vislump, L[7] + off, visbytes );
			}
		}
	}

	// ---- leaves + marksurfaces (world leaves only)
	size_t cms_cap = 0;
	ol = (outleaf_t *)malloc( sizeof( outleaf_t ) * ( nleaves_out ? nleaves_out : 1 ));
	for( int i = 0; i < nleaves_out; i++ )
	{
		const unsigned char *lp = L[11] + i * 48;
		int type = rd_i32( lp );
		int lsi = rd_i32( lp + 32 );
		int lsc = rd_i32( lp + 36 );
		int first = (int)nms;
		for( int k = 0; k < lsc; k++ )
		{
			int si = rd_i32( L[12] + ( lsi + k ) * 4 );
			if( si < 0 || si >= nsurfaces ) continue;
			int start = surf_first[si], cnt = surf_count[si];
			for( int f = 0; f < cnt; f++ )
			{
				if( nms + 1 > cms_cap ) { cms_cap = cms_cap ? cms_cap * 2 : 16384; oms = (unsigned short *)realloc( oms, cms_cap * 2 ); }
				oms[nms++] = (unsigned short)( start + f );
			}
		}
		ol[i].contents = ( type == 2 ) ? -2 : -1;
		ol[i].visofs = leaf_visofs[i];
		ol[i].mins[0] = (short)clamp_short( rd_f32( lp + 8 ));
		ol[i].mins[1] = (short)clamp_short( rd_f32( lp + 12 ));
		ol[i].mins[2] = (short)clamp_short( rd_f32( lp + 16 ));
		ol[i].maxs[0] = (short)clamp_short( rd_f32( lp + 20 ));
		ol[i].maxs[1] = (short)clamp_short( rd_f32( lp + 24 ));
		ol[i].maxs[2] = (short)clamp_short( rd_f32( lp + 28 ));
		ol[i].firstms = first;
		ol[i].numms = (int)nms - first;
	}

	// ---- nodes
	on = (outnode_t *)malloc( sizeof( outnode_t ) * ( nnodes ? nnodes : 1 ));
	for( int i = 0; i < nnodes; i++ )
	{
		const unsigned char *np = L[8] + i * 36;
		on[i].planenum = rd_i32( np );
		on[i].c0 = rd_i32( np + 4 );
		on[i].c1 = rd_i32( np + 8 );
		on[i].mins[0] = (short)clamp_short( rd_f32( np + 12 ));
		on[i].mins[1] = (short)clamp_short( rd_f32( np + 16 ));
		on[i].mins[2] = (short)clamp_short( rd_f32( np + 20 ));
		on[i].maxs[0] = (short)clamp_short( rd_f32( np + 24 ));
		on[i].maxs[1] = (short)clamp_short( rd_f32( np + 28 ));
		on[i].maxs[2] = (short)clamp_short( rd_f32( np + 32 ));
		on[i].firstface = 0; on[i].numfaces = 0;
	}

	// ---- models
	om = (outmodel_t *)malloc( sizeof( outmodel_t ) * ( nmodels ? nmodels : 1 ));
	for( int i = 0; i < nmodels; i++ )
	{
		const unsigned char *mp = L[14] + i * 56;
		for( int k = 0; k < 3; k++ ) om[i].mins[k] = rd_f32( mp + k * 4 );
		for( int k = 0; k < 3; k++ ) om[i].maxs[k] = rd_f32( mp + 12 + k * 4 );
		om[i].origin[0] = om[i].origin[1] = om[i].origin[2] = 0;
		for( int k = 0; k < 4; k++ ) om[i].headnode[k] = 0;
		// BSP42 model: mins[3], maxs[3], hulls[4], leavesIndex, leavesCount,
		//               surfacesIndex, surfacesCount
		int msi = rd_i32( mp + 48 );		// surfacesIndex
		int msc = rd_i32( mp + 52 );		// surfacesCount
		// world visleafs must equal the emitted leaf count (see above)
		om[i].visleafs = ( i == 0 ) ? nleaves_out : rd_i32( mp + 44 );
		if( msc > 0 && msi < nsurfaces )
		{
			int first = surf_first[msi];
			int end_surf = msi + msc;
			int end = ( end_surf < nsurfaces ) ? surf_first[end_surf] : (int)nof;
			// Some surfaces inside the range may have had no faces; recompute the
			// end as first-face-of-next *present* surface.
			if( end < first ) end = (int)nof;
			om[i].firstface = first;
			om[i].numfaces = end - first;
		}
		else { om[i].firstface = 0; om[i].numfaces = 0; }
	}

	// ---- assemble BSP30
	buf_t out = { 0 };
	buf_t lumps[BSP30_LUMPS];
	memset( lumps, 0, sizeof( lumps ));

	// entities, with the full Nightfire texture table appended to worldspawn.
	// BSP30 miptex names are limited to 16 bytes, but Nightfire texture paths
	// ("caviar/metal_aluminumrust_02") routinely exceed that and collide when
	// truncated, so we carry them verbatim. The table is split across short
	// "_nftexN" keys so game-DLL entity parsers (which use fixed token buffers)
	// never see a huge value; the engine concatenates them in order.
	{
		const unsigned char *ents = L[0];
		int elen = llen[0];
		int cut = 0;
		int chunk = 0, first = 1;
		size_t clen = 0;
		char hdr[32];
		buf_t key = { 0 };

		while( cut < elen && ents[cut] != '}' )
			cut++;

		for( int i = 0; i < ntextures; i++ )
		{
			const unsigned char *tp = L[2] + i * 64;
			int len = 0;
			while( len < 64 && tp[len] )
				len++;
			if( !len ) { tp = (const unsigned char *)"_"; len = 1; } // keep index alignment

			if( first || clen + (size_t)len + 1 > 180 )
			{
				if( !first ) buf_bytes( &key, "\"\n", 2 );
				int n = snprintf( hdr, sizeof( hdr ), "\"_nftex%d\" \"", chunk++ );
				buf_bytes( &key, hdr, n );
				clen = 0; first = 0;
			}
			else
			{
				buf_bytes( &key, " ", 1 );
				clen++;
			}
			buf_bytes( &key, tp, len );
			clen += len;
		}
		if( !first )
			buf_bytes( &key, "\"\n", 2 );

		buf_bytes( &lumps[0], ents, cut );
		buf_bytes( &lumps[0], key.d, key.len );
		buf_bytes( &lumps[0], ents + cut, elen - cut );
		free( key.d );
	}
	// planes
	for( int i = 0; i < nplanes; i++ )
	{
		const unsigned char *p = L[1] + i * 20;
		float nx = rd_f32( p ), ny = rd_f32( p + 4 ), nz = rd_f32( p + 8 );
		buf_f32( &lumps[1], nx ); buf_f32( &lumps[1], ny ); buf_f32( &lumps[1], nz );
		buf_f32( &lumps[1], rd_f32( p + 12 ));
		buf_i32( &lumps[1], nf_plane_type( nx, ny, nz ));
	}
	// textures: miptex placeholders
	{
		int n = ntextures;
		buf_i32( &lumps[2], n );
		int dataofs = 4 + 4 * n;
		for( int i = 0; i < n; i++ ) { buf_i32( &lumps[2], dataofs ); dataofs += 40; }
		for( int i = 0; i < n; i++ )
		{
			char name[16];
			int len = 0;

			memset( name, 0, 16 );
			while( len < 64 && L[2][i * 64 + len] )
				len++;

			if( tex_masked[i] )
			{
				// GoldSrc masked textures are prefixed with '{'
				name[0] = '{';
				if( len > 14 ) len = 14;
				memcpy( name + 1, L[2] + i * 64, len );
			}
			else
			{
				if( len > 15 ) len = 15;
				memcpy( name, L[2] + i * 64, len );
			}

			buf_bytes( &lumps[2], name, 16 );
			buf_i32( &lumps[2], 0 ); buf_i32( &lumps[2], 0 );   // width, height
			buf_i32( &lumps[2], 0 ); buf_i32( &lumps[2], 0 );   // offsets 0,1
			buf_i32( &lumps[2], 0 ); buf_i32( &lumps[2], 0 );   // offsets 2,3
		}
	}
	// vertices
	for( size_t i = 0; i < nov; i++ ) buf_f32( &lumps[3], ov[i] );
	// visibility (transferred from vislump; freed with the other lumps)
	lumps[4] = vislump;
	// nodes
	for( int i = 0; i < nnodes; i++ )
	{
		buf_i32( &lumps[5], on[i].planenum );
		buf_i16( &lumps[5], (short)on[i].c0 ); buf_i16( &lumps[5], (short)on[i].c1 );
		for( int k = 0; k < 3; k++ ) buf_i16( &lumps[5], on[i].mins[k] );
		for( int k = 0; k < 3; k++ ) buf_i16( &lumps[5], on[i].maxs[k] );
		buf_u16( &lumps[5], on[i].firstface ); buf_u16( &lumps[5], on[i].numfaces );
	}
	// texinfo
	for( size_t i = 0; i < noti; i++ )
	{
		for( int k = 0; k < 4; k++ ) buf_f32( &lumps[6], oti[i].s[k] );
		for( int k = 0; k < 4; k++ ) buf_f32( &lumps[6], oti[i].t[k] );
		buf_i32( &lumps[6], oti[i].miptex ); buf_i32( &lumps[6], oti[i].flags );
	}
	// faces
	for( size_t i = 0; i < nof; i++ )
	{
		buf_u16( &lumps[7], (unsigned short)of[i].plane );
		buf_i16( &lumps[7], of[i].side );
		buf_i32( &lumps[7], of[i].firstedge );
		buf_i16( &lumps[7], (short)of[i].numedges );
		buf_i16( &lumps[7], (short)of[i].texinfo );
		if( of[i].lightofs >= 0 )
		{
			unsigned char st[4] = { 0, 255, 255, 255 };	// light style 0
			buf_bytes( &lumps[7], st, 4 );
		}
		else
		{
			unsigned char st[4] = { 255, 255, 255, 255 };	// no lightmap
			buf_bytes( &lumps[7], st, 4 );
		}
		buf_i32( &lumps[7], of[i].lightofs );
	}
	// lighting (resampled from the Nightfire per-surface lightmaps)
	lumps[8] = lighting;
	// clipnodes (hulls 1-3): Nightfire has no clipnode lump (its collision is
	// surface-based). Build the world's three hulls as expanded solid BSPs over
	// the world brushes (see nfh_build_world / docs/world-clip-hull.md); the
	// previous render-tree copy was unexpanded and is no longer emitted.
	{
#ifdef NFBSP_WORLD_HULLS
		int nbrushes_w = llen[15] / 12;

		if( !nfw_build_world( L, nleaves, nbrushes_w, llen[1] / 20, &lumps[9], &lumps[1], om[0].headnode ))
		{
			om[0].headnode[1] = 0;
			om[0].headnode[2] = 0;
			om[0].headnode[3] = 0;
		}
#else
		om[0].headnode[1] = 0;
		om[0].headnode[2] = 0;
		om[0].headnode[3] = 0;
#endif
	}

	// Brush-model clip trees for hulls 1-3, expanded per hull. A model's solid
	// volume is the union of its convex brushes; that union is exactly encoded
	// by chaining each brush's planes (front -> next brush, back -> next plane,
	// last back -> CONTENTS_SOLID). Expansion offsets each plane outward by the
	// hull's support (Minkowski sum with the hull bbox).
	{
		static const float hmins[4][3] = { {0,0,0}, {-16,-16,-36}, {-32,-32,-32}, {-16,-16,-18} };
		static const float hmaxs[4][3] = { {0,0,0}, {16,16,36}, {32,32,32}, {16,16,18} };
		const int nbrushes = llen[15] / 12;
		unsigned char *seen = (unsigned char *)calloc( nbrushes ? nbrushes : 1, 1 );
		int *order = (int *)malloc( sizeof( int ) * ( nbrushes ? nbrushes : 1 ));

		for( int mi = 1; mi < nmodels && seen && order; mi++ )
		{
			int lindex = rd_i32( L[14] + mi * 56 + 40 );
			int lcount = rd_i32( L[14] + mi * 56 + 44 );
			int cnt = 0;

			if( lindex < 0 || lcount <= 0 )
				continue;

			for( int lk = 0; lk < lcount; lk++ )
			{
				int li = lindex + lk, lbi, lbc;
				const unsigned char *lp;

				if( li < 0 || li >= nleaves )
					continue;
				lp = L[11] + li * 48;
				lbi = rd_i32( lp + 40 );
				lbc = rd_i32( lp + 44 );
				if( lbi < 0 || lbc <= 0 )
					continue;

				for( int k = 0; k < lbc; k++ )
				{
					int bi = rd_i32( L[13] + ( lbi + k ) * 4 );
					if( bi >= 0 && bi < nbrushes && !seen[bi] )
					{
						seen[bi] = 1;
						order[cnt++] = bi;
					}
				}
			}

			// skip a model whose three hulls would overflow 16-bit clipnodes
			{
				int need = 0;
				for( int k = 0; k < cnt; k++ )
					need += 3 * rd_i32( L[15] + order[k] * 12 + 8 );
				if( (int)( lumps[9].len / 8 ) + need > 32760 )
				{
					for( int k = 0; k < cnt; k++ ) seen[order[k]] = 0;
					continue;
				}
			}

			for( int hull = 1; hull <= 3; hull++ )
			{
				int rest = -1;	// CONTENTS_EMPTY

				for( int k = cnt - 1; k >= 0; k-- )
				{
					int bi = order[k];
					int si = rd_i32( L[15] + bi * 12 + 4 );
					int sc = rd_i32( L[15] + bi * 12 + 8 );
					int back = -2;	// CONTENTS_SOLID

					for( int j = sc - 1; j >= 0; j-- )
					{
						int pl = rd_i32( L[16] + ( si + j ) * 8 + 4 );
						const unsigned char *pp;
						float nx, ny, nz, dd, sup;
						int newpl, idx;

						if( pl < 0 || pl >= nplanes )
							continue;
						pp = lumps[1].d + pl * 20;
						nx = rd_f32( pp ); ny = rd_f32( pp + 4 );
						nz = rd_f32( pp + 8 ); dd = rd_f32( pp + 12 );
						sup = ( nx > 0 ? nx * hmaxs[hull][0] : nx * hmins[hull][0] )
						    + ( ny > 0 ? ny * hmaxs[hull][1] : ny * hmins[hull][1] )
						    + ( nz > 0 ? nz * hmaxs[hull][2] : nz * hmins[hull][2] );

						newpl = (int)( lumps[1].len / 20 );
						buf_f32( &lumps[1], nx ); buf_f32( &lumps[1], ny );
						buf_f32( &lumps[1], nz ); buf_f32( &lumps[1], dd + sup );
						buf_i32( &lumps[1], nf_plane_type( nx, ny, nz ));

						idx = (int)( lumps[9].len / 8 );
						buf_i32( &lumps[9], newpl );
						buf_i16( &lumps[9], (short)rest );
						buf_i16( &lumps[9], (short)back );
						back = idx;
					}
					rest = back;
				}

				om[mi].headnode[hull] = ( rest >= 0 && rest < 32768 ) ? rest : 0;
			}

			for( int k = 0; k < cnt; k++ )
				seen[order[k]] = 0;
		}

		free( seen ); free( order );
	}
	// leaves
	for( int i = 0; i < nleaves_out; i++ )
	{
		buf_i32( &lumps[10], ol[i].contents );
		buf_i32( &lumps[10], ol[i].visofs );
		for( int k = 0; k < 3; k++ ) buf_i16( &lumps[10], ol[i].mins[k] );
		for( int k = 0; k < 3; k++ ) buf_i16( &lumps[10], ol[i].maxs[k] );
		buf_u16( &lumps[10], (unsigned short)ol[i].firstms );
		buf_u16( &lumps[10], (unsigned short)ol[i].numms );
		{ unsigned char amb[4] = { 0, 0, 0, 0 }; buf_bytes( &lumps[10], amb, 4 ); }
	}
	// marksurfaces
	for( size_t i = 0; i < nms; i++ ) buf_u16( &lumps[11], oms[i] );
	// edges
	for( size_t i = 0; i < noe; i++ )
	{
		buf_u16( &lumps[12], oe0[i] ); buf_u16( &lumps[12], oe1[i] );
		buf_i32( &lumps[12], 0 );
	}
	// surfedges
	for( size_t i = 0; i < nose; i++ ) buf_i32( &lumps[13], ose[i] );
	// models
	for( int i = 0; i < nmodels; i++ )
	{
		for( int k = 0; k < 3; k++ ) buf_f32( &lumps[14], om[i].mins[k] );
		for( int k = 0; k < 3; k++ ) buf_f32( &lumps[14], om[i].maxs[k] );
		for( int k = 0; k < 3; k++ ) buf_f32( &lumps[14], om[i].origin[k] );
		for( int k = 0; k < 4; k++ ) buf_i32( &lumps[14], om[i].headnode[k] );
		buf_i32( &lumps[14], om[i].visleafs );
		buf_i32( &lumps[14], om[i].firstface );
		buf_i32( &lumps[14], om[i].numfaces );
	}

	// header
	buf_i32( &out, HLBSP_VERSION );
	size_t hdr = out.len;
	for( int i = 0; i < BSP30_LUMPS; i++ ) { buf_i32( &out, 0 ); buf_i32( &out, 0 ); }
	for( int i = 0; i < BSP30_LUMPS; i++ )
	{
		buf_patch_i32( &out, hdr + i * 8 + 0, (int)out.len );
		buf_patch_i32( &out, hdr + i * 8 + 4, (int)lumps[i].len );
		buf_bytes( &out, lumps[i].d, lumps[i].len );
	}

	// cleanup
	for( int i = 0; i < BSP30_LUMPS; i++ ) free( lumps[i].d );
	free( ov ); free( oe0 ); free( oe1 ); free( ose ); free( of );
	free( oms ); free( ol ); free( on ); free( om ); free( oti );
	free( surf_first ); free( surf_count );
	free( tex_masked ); free( surf_skip ); free( leaf_visofs );
	map_free( &vmap ); map_free( &ti_map );
	// note: vislump.d was moved into lumps[4] and is freed above

	*outsize = out.len;
	return (byte *)out.d;

	#undef PUSH_VERT
	#undef PUSH_EDGE
	#undef PUSH_SURFEDGE
	#undef PUSH_FACE
}
