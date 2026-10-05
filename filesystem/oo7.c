/*
oo7.c - Gearbox "007" asset-pack support for the Xash3D filesystem.

James Bond 007: Nightfire (PC) stores almost all of its content in
bond/assets.007, an "OO7" container. This provider mounts that archive as a
read-only search path so the engine can find maps, models, textures, sounds
and scripts by their normal paths.

Format (little-endian), reverse engineered from the retail archive and
cross-checked against sourcepp's vpkpp OO7 reader:

	u32 majorVersion (=1)
	u32 minorVersion (=1 or 3)

	tree, read recursively:
	    u32 nameLen; char name[nameLen]
	    u32 subDirCount
	    if minorVersion == 3: u32 fileCount
	    repeat until u32 filenameSize == 0:
	        u32 filenameSize; char filename[filenameSize]
	        u8  compressed
	        u32 length                      (uncompressed size)
	        u32 other                       (compressed size, or == length)
	    then recurse into subDirCount subdirectories

	v1.3: after the tree, entries' bytes are laid out contiguously in tree
	traversal order (this is what we compute offsets from). v1.1 interleaves
	each entry's data after its header; both are handled here.

A compressed entry is a zlib stream (inflate with windowBits = MAX_WBITS).

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#if XASH_POSIX
#include <unistd.h>
#endif
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include "port.h"
#include "filesystem_internal.h"
#include "crtlib.h"
#include "common/com_strings.h"

#define OO7_HEADER_VERSION 1u
#define OO7_MAX_ENTRIES    131072
#define OO7_MAX_DEPTH      64

#define OO7_FLAG_COMPRESSED BIT( 0 )

typedef struct oo7file_s
{
	char        name[MAX_SYSPATH];
	fs_offset_t offset;          // offset in the archive
	fs_offset_t size;            // uncompressed size
	fs_offset_t compressed_size; // 0 if stored
	uint16_t    flags;
} oo7file_t;

struct oo7_s
{
	file_t    *handle;
	int       numfiles;
	uint32_t  minor;
	oo7file_t files[]; // flexible
};

// temporary collection while parsing (before we know the final count)
typedef struct oo7tmp_s
{
	oo7file_t *items;
	int        count;
	int        cap;
} oo7tmp_t;

/*
============
OO7_ReadU32
============
*/
static qboolean OO7_ReadU32( file_t *f, uint32_t *out )
{
	return FS_Read( f, out, sizeof( *out )) == sizeof( *out );
}

/*
============
OO7_SortFiles
============
*/
static int OO7_SortFiles( const void *a, const void *b )
{
	return Q_stricmp(((const oo7file_t *)a)->name, ((const oo7file_t *)b)->name );
}

/*
============
OO7_ReadTree

Recursively parse one directory node. File entries are appended to tmp in
traversal order (which is the order v1.3 lays out the data section).
============
*/
static qboolean OO7_ReadTree( file_t *f, uint32_t minor, const char *parent, qboolean root, oo7tmp_t *tmp, int depth )
{
	char     current[MAX_SYSPATH];
	char     name[MAX_SYSPATH];
	uint32_t name_len, subdir_count;

	if( depth > OO7_MAX_DEPTH )
		return false;

	if( !OO7_ReadU32( f, &name_len ) || name_len >= MAX_SYSPATH )
		return false;
	if( FS_Read( f, name, name_len ) != (fs_offset_t)name_len )
		return false;
	name[name_len] = '\0';

	if( root )
		current[0] = '\0';
	else if( parent[0] )
		Q_snprintf( current, sizeof( current ), "%s/%s", parent, name );
	else
		Q_strncpy( current, name, sizeof( current ));

	if( !OO7_ReadU32( f, &subdir_count ))
		return false;
	if( minor == 3 )
	{
		uint32_t file_count;
		if( !OO7_ReadU32( f, &file_count ))
			return false;
	}

	for( ;; )
	{
		uint32_t   fn_len, length, other;
		uint8_t    compressed;
		char       shortname[MAX_SYSPATH];
		oo7file_t *e;

		if( !OO7_ReadU32( f, &fn_len ))
			return false;
		if( fn_len == 0 )
			break;
		if( fn_len >= MAX_SYSPATH )
			return false;
		if( FS_Read( f, shortname, fn_len ) != (fs_offset_t)fn_len )
			return false;
		shortname[fn_len] = '\0';

		if( FS_Read( f, &compressed, sizeof( compressed )) != sizeof( compressed ))
			return false;
		if( !OO7_ReadU32( f, &length ) || !OO7_ReadU32( f, &other ))
			return false;

		if( tmp->count >= OO7_MAX_ENTRIES )
			return false;
		if( tmp->count == tmp->cap )
		{
			int ncap = tmp->cap ? tmp->cap * 2 : 4096;
			oo7file_t *ni = (oo7file_t *)Mem_Realloc( fs_mempool, tmp->items, sizeof( *ni ) * ncap );
			if( !ni )
				return false;
			tmp->items = ni;
			tmp->cap = ncap;
		}

		e = &tmp->items[tmp->count++];
		memset( e, 0, sizeof( *e ));
		if( current[0] )
			Q_snprintf( e->name, sizeof( e->name ), "%s/%s", current, shortname );
		else
			Q_strncpy( e->name, shortname, sizeof( e->name ));
		e->size = length;
		e->compressed_size = compressed ? other : 0;
		e->flags = compressed ? OO7_FLAG_COMPRESSED : 0;

		// v1.1 stores each entry's data inline, right after its header
		if( minor == 1 )
		{
			e->offset = FS_Tell( f );
			FS_Seek( f, compressed ? (fs_offset_t)other : (fs_offset_t)length, SEEK_CUR );
		}
	}

	for( uint32_t i = 0; i < subdir_count; i++ )
	{
		if( !OO7_ReadTree( f, minor, current, false, tmp, depth + 1 ))
			return false;
	}

	return true;
}

/*
============
FS_LoadOO7
============
*/
static oo7_t *FS_LoadOO7( const char *oo7file, int *error )
{
	oo7_t   *oo7 = NULL;
	oo7tmp_t tmp = { NULL, 0, 0 };
	uint32_t major = 0, minor = 0;
	file_t  *handle;

	handle = FS_SysOpen( oo7file, "rb" );
	if( !handle )
	{
		Con_Reportf( S_ERROR "%s couldn't open\n", oo7file );
		if( error ) *error = 1;
		return NULL;
	}

	if( !OO7_ReadU32( handle, &major ) || !OO7_ReadU32( handle, &minor ) ||
		major != OO7_HEADER_VERSION || ( minor != 1 && minor != 3 ))
	{
		Con_Reportf( S_ERROR "%s is not a Nightfire 007 archive\n", oo7file );
		if( error ) *error = 2;
		FS_Close( handle );
		return NULL;
	}

	if( !OO7_ReadTree( handle, minor, "", true, &tmp, 0 ) || tmp.count == 0 )
	{
		Con_Reportf( S_ERROR "%s: failed to parse directory tree\n", oo7file );
		if( error ) *error = 3;
		Mem_Free( tmp.items );
		FS_Close( handle );
		return NULL;
	}

	// v1.3: assign data offsets, laid out contiguously after the tree.
	if( minor == 3 )
	{
		fs_offset_t pos = FS_Tell( handle );
		for( int i = 0; i < tmp.count; i++ )
		{
			tmp.items[i].offset = pos;
			pos += tmp.items[i].compressed_size ? tmp.items[i].compressed_size : tmp.items[i].size;
		}
	}

	oo7 = (oo7_t *)Mem_Calloc( fs_mempool, sizeof( *oo7 ) + sizeof( oo7file_t ) * tmp.count );
	if( !oo7 )
	{
		if( error ) *error = 4;
		Mem_Free( tmp.items );
		FS_Close( handle );
		return NULL;
	}

	oo7->handle = handle;
	oo7->numfiles = tmp.count;
	oo7->minor = minor;
	memcpy( oo7->files, tmp.items, sizeof( oo7file_t ) * tmp.count );
	Mem_Free( tmp.items );

	// case-insensitive lookup uses binary search, so keep the list sorted
	qsort( oo7->files, oo7->numfiles, sizeof( oo7->files[0] ), OO7_SortFiles );

	if( error ) *error = 0;
	return oo7;
}

/*
============
FS_CloseOO7
============
*/
static void FS_CloseOO7( oo7_t *oo7 )
{
	if( oo7->handle != NULL )
		FS_Close( oo7->handle );

	Mem_Free( oo7 );
}

/*
============
FS_Close_OO7
============
*/
static void FS_Close_OO7( searchpath_t *search )
{
	FS_CloseOO7( search->oo7 );
}

/*
============
FS_OpenFile_OO7
============
*/
static file_t *FS_OpenFile_OO7( searchpath_t *search, const char *filename, const char *mode, int pack_ind )
{
	oo7file_t *pfile = &search->oo7->files[pack_ind];
	file_t *f = FS_OpenHandle( search, search->oo7->handle->handle, pfile->offset, pfile->size );

	if( !f )
		return NULL;

	if( FBitSet( pfile->flags, OO7_FLAG_COMPRESSED ))
	{
		ztoolkit_t *ztk;

		SetBits( f->flags, FILE_DEFLATED );

		ztk = (ztoolkit_t *)Mem_Calloc( fs_mempool, sizeof( *ztk ));
		ztk->comp_length = pfile->compressed_size;
		ztk->zstream.next_in = ztk->input;
		ztk->zstream.avail_in = 0;

		// OO7 uses the zlib container format (windowBits = MAX_WBITS)
		if( inflateInit2( &ztk->zstream, MAX_WBITS ) != Z_OK )
		{
			Con_Printf( "%s: inflate init error (file: %s)\n", __func__, filename );
			FS_Close( f );
			Mem_Free( ztk );
			return NULL;
		}

		ztk->zstream.next_out = f->buff;
		ztk->zstream.avail_out = sizeof( f->buff );

		f->ztk = ztk;
	}

	return f;
}

/*
============
FS_LoadOO7File
============
*/
static byte *FS_LoadOO7File( searchpath_t *search, const char *path, int pack_ind, fs_offset_t *sizeptr, void *( *pfnAlloc )( size_t ), void ( *pfnFree )( void * ))
{
	oo7file_t *file = &search->oo7->files[pack_ind];
	byte *decompressed_buffer;

	if( sizeptr ) *sizeptr = 0;

	if( FS_Seek( search->oo7->handle, file->offset, SEEK_SET ) == -1 )
		return NULL;

	decompressed_buffer = (byte *)pfnAlloc( file->size + 1 );
	if( unlikely( !decompressed_buffer ))
	{
		Con_Reportf( S_ERROR "%s: can't alloc %li bytes\n", __func__, (long)file->size + 1 );
		return NULL;
	}
	decompressed_buffer[file->size] = '\0';

	if( !FBitSet( file->flags, OO7_FLAG_COMPRESSED ))
	{
		if( FS_Read( search->oo7->handle, decompressed_buffer, file->size ) != file->size )
		{
			Con_Reportf( S_ERROR "%s: %s size doesn't match\n", __func__, file->name );
			pfnFree( decompressed_buffer );
			return NULL;
		}

		if( sizeptr ) *sizeptr = file->size;
		return decompressed_buffer;
	}
	else
	{
		byte *compressed_buffer = (byte *)Mem_Malloc( fs_mempool, file->compressed_size );
		int zlib_result;
		z_stream decompress_stream =
		{
			.total_in = file->compressed_size,
			.avail_in = file->compressed_size,
			.next_in = (Bytef *)compressed_buffer,
			.total_out = file->size,
			.avail_out = file->size,
			.next_out = (Bytef *)decompressed_buffer,
			.zalloc = Z_NULL,
			.zfree = Z_NULL,
			.opaque = Z_NULL,
		};

		if( !compressed_buffer || FS_Read( search->oo7->handle, compressed_buffer, file->compressed_size ) != file->compressed_size )
		{
			Mem_Free( compressed_buffer );
			pfnFree( decompressed_buffer );
			return NULL;
		}

		if( inflateInit2( &decompress_stream, MAX_WBITS ) != Z_OK )
		{
			Mem_Free( compressed_buffer );
			pfnFree( decompressed_buffer );
			return NULL;
		}

		zlib_result = inflate( &decompress_stream, Z_NO_FLUSH );
		inflateEnd( &decompress_stream );
		Mem_Free( compressed_buffer );

		if( zlib_result != Z_OK && zlib_result != Z_STREAM_END )
		{
			Con_Reportf( S_ERROR "%s: %s: zlib error %d\n", __func__, file->name, zlib_result );
			pfnFree( decompressed_buffer );
			return NULL;
		}

		if( sizeptr ) *sizeptr = file->size;
		return decompressed_buffer;
	}
}

/*
============
FS_FileTime_OO7
============
*/
static int FS_FileTime_OO7( searchpath_t *search, const char *filename )
{
	return search->oo7->handle->filetime;
}

/*
============
FS_PrintInfo_OO7
============
*/
static void FS_PrintInfo_OO7( searchpath_t *search, char *dst, size_t size )
{
	Q_snprintf( dst, size, "%s (%i files)", search->filename, search->oo7->numfiles );
}

/*
============
FS_FindFile_OO7
============
*/
static int FS_FindFile_OO7( searchpath_t *search, const char *path, char *fixedname, size_t len )
{
	int left = 0;
	int right = search->oo7->numfiles - 1;

	while( left <= right )
	{
		int middle = (left + right) / 2;
		int diff = Q_stricmp( search->oo7->files[middle].name, path );

		if( !diff )
		{
			if( fixedname )
				Q_strncpy( fixedname, search->oo7->files[middle].name, len );
			return middle;
		}

		if( diff > 0 )
			right = middle - 1;
		else
			left = middle + 1;
	}

	return -1;
}

/*
============
FS_Search_OO7
============
*/
static void FS_Search_OO7( searchpath_t *search, stringlist_t *list, const char *pattern, int caseinsensitive )
{
	string temp;

	for( int i = 0; i < search->oo7->numfiles; i++ )
	{
		Q_strncpy( temp, search->oo7->files[i].name, sizeof( temp ));
		while( temp[0] )
		{
			const char *slash, *backslash, *colon, *separator;

			if( matchpattern( temp, pattern, true ))
			{
				int j;

				for( j = 0; j < list->numstrings; j++ )
				{
					if( !Q_strcmp( list->strings[j], temp ))
						break;
				}

				if( j == list->numstrings )
					stringlistappend( list, temp );
			}

			slash = Q_strrchr( temp, '/' );
			backslash = Q_strrchr( temp, '\\' );
			colon = Q_strrchr( temp, ':' );
			separator = temp;
			if( separator < slash )
				separator = slash;
			if( separator < backslash )
				separator = backslash;
			if( separator < colon )
				separator = colon;
			*((char *)separator) = 0;
		}
	}
}

/*
============
FS_AddOO7_Fullpath
============
*/
searchpath_t *FS_AddOO7_Fullpath( const char *oo7file, int flags )
{
	searchpath_t *search;
	int errorcode = 1;
	oo7_t *oo7 = FS_LoadOO7( oo7file, &errorcode );

	if( !oo7 )
	{
		Con_Reportf( S_ERROR "%s: unable to load Nightfire 007 archive \"%s\"\n", __func__, oo7file );
		return NULL;
	}

	search = (searchpath_t *)Mem_Calloc( fs_mempool, sizeof( searchpath_t ));
	Q_strncpy( search->filename, oo7file, sizeof( search->filename ));
	search->oo7 = oo7;
	search->type = SEARCHPATH_OO7;
	search->flags = flags;

	search->pfnPrintInfo = FS_PrintInfo_OO7;
	search->pfnClose = FS_Close_OO7;
	search->pfnOpenFile = FS_OpenFile_OO7;
	search->pfnFileTime = FS_FileTime_OO7;
	search->pfnFindFile = FS_FindFile_OO7;
	search->pfnSearch = FS_Search_OO7;
	search->pfnLoadFile = FS_LoadOO7File;

	Con_Reportf( "Adding Nightfire 007 archive: %s (%i files)\n", oo7file, oo7->numfiles );
	return search;
}
