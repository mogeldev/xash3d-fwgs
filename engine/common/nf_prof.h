/*
nf_prof.h - James Bond 007: Nightfire port frame profiler

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#ifndef NF_PROF_H
#define NF_PROF_H

// frame sections, measured without overlap; the rest of the frame is "other"
typedef enum
{
	NFP_INPUT = 0, // input, client begin, dedicated commands
	NFP_SERVER,    // Host_ServerFrame
	NFP_CLDLL,     // client.dll HUD_Frame
	NFP_NET,       // send command, read packets, prediction
	NFP_EMIT,      // CL_EmitEntities
	NFP_WORLD,     // V_RenderView (3D view)
	NFP_MOVIE,     // AVI_Think (decode + upload)
	NFP_2D,        // V_PostRender up to the swap (HUD, menu, console)
	NFP_SHOT,      // SCR_MakeScreenShot (writing a screenshot)
	NFP_SWAP,      // R_EndFrame (buffer swap, vsync wait)
	NFP_SOUND,     // SND_UpdateSound
	NFP_COUNT
} nfp_section_t;

// what the frame showed, the report splits by it
#define NFP_STATE_OTHER 0 // menu, loading, console
#define NFP_STATE_GAME  1 // in game
#define NFP_STATE_MOVIE 2 // start-up or in-game movie

extern convar_t nf_prof;

void NF_ProfInit( void );
void NF_ProfFrameBegin( int state );
void NF_ProfFrameEnd( void );
void NF_ProfSectionBegin( nfp_section_t s );
void NF_ProfSectionEnd( nfp_section_t s );
void NF_ProfFileLoad( const char *name, double start, qboolean found );
void NF_ProfMovieFrames( int decoded, qboolean shown );

#define NFP_BEGIN( s ) do { if( nf_prof.value ) NF_ProfSectionBegin( s ); } while( 0 )
#define NFP_END( s ) do { if( nf_prof.value ) NF_ProfSectionEnd( s ); } while( 0 )

#endif // NF_PROF_H
