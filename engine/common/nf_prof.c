/*
nf_prof.c - James Bond 007: Nightfire port frame profiler

nf_prof 1 measures every host frame in sections (nf_prof.h) and
prints the frames that take longer than nf_prof_spike milliseconds,
with the sections and the files loaded in that frame. nf_prof 2 also
prints every file load while in game or in a movie. nf_prof_report
prints average / p95 / p99 / max of the frames recorded since
nf_prof_reset, split into in-game and movie frames, plus the pacing of
the movie's video frames (shown, skipped, gaps). Frames that write a
screenshot are left out. Only measures, never changes behaviour

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "common.h"
#include "nf_prof.h"
#include "platform/platform.h"

CVAR_DEFINE_AUTO( nf_prof, "0", 0, "Nightfire port frame profiler: 1 log spike frames, 2 also log every file load in game" );
static CVAR_DEFINE_AUTO( nf_prof_spike, "25", 0, "nf_prof: frames longer than this many milliseconds are logged" );

#define NFP_HISTORY   8192
#define NFP_MAX_LOADS 8

static const char *nfp_names[NFP_COUNT] =
{
	"input", "server", "cldll", "net", "emit", "world", "movie", "2d", "shot", "swap", "sound"
};

typedef struct
{
	float interval; // ms from this frame's start to the next one's (what the player sees)
	float work;     // ms spent inside Host_Frame
	float sections[NFP_COUNT];
	char  state;    // NFP_STATE_*
} nfp_frame_t;

typedef struct
{
	char  name[64];
	float ms;
	qboolean found;
} nfp_load_t;

static struct
{
	qboolean   inframe;
	qboolean   have_prev; // cur holds a finished frame waiting for its interval
	double     frame_start;
	double     frame_end;
	double     section_start[NFP_COUNT];
	nfp_frame_t cur;
	nfp_load_t loads[NFP_MAX_LOADS];
	int        numloads;  // may be more than NFP_MAX_LOADS
	float      loadms;
	unsigned int framenum;

	nfp_frame_t history[NFP_HISTORY];
	int        numhistory; // total recorded, the ring keeps the last NFP_HISTORY
	int        numshots;   // screenshot frames, left out of the history

	// movie pacing (AVI_Think of the main movie)
	int        mv_shown;   // video frames put on screen
	int        mv_skipped; // decoded but replaced by a later one in the same call
	double     mv_last;    // when the last one was put on screen
	float      mv_gapsum;  // ms between two shown frames
	float      mv_gapmax;
	int        mv_gaps;
	int        mv_late;    // gaps over 50 ms (1.5 frames at 30 fps)
} nfp;

static void NF_ProfPrintFrame( const nfp_frame_t *f )
{
	string line;
	int len, i;
	float sum = 0.0f;

	len = Q_snprintf( line, sizeof( line ), "nf_prof: spike frame %u (%s) %.1f ms, work %.1f:",
		nfp.framenum, f->state == NFP_STATE_GAME ? "game" : f->state == NFP_STATE_MOVIE ? "movie" : "other",
		f->interval, f->work );

	for( i = 0; i < NFP_COUNT && len > 0 && len < sizeof( line ); i++ )
	{
		sum += f->sections[i];
		if( f->sections[i] >= 0.1f )
			len += Q_snprintf( line + len, sizeof( line ) - len, " %s %.1f", nfp_names[i], f->sections[i] );
	}

	if( len > 0 && len < sizeof( line ))
		Q_snprintf( line + len, sizeof( line ) - len, " other %.1f sleep %.1f", f->work - sum, f->interval - f->work );

	Con_Printf( "%s\n", line );

	if( !nfp.numloads )
		return;

	Con_Printf( "nf_prof:   %d file loads, %.1f ms:", nfp.numloads, nfp.loadms );
	for( i = 0; i < nfp.numloads && i < NFP_MAX_LOADS; i++ )
		Con_Printf( " %s %.1f%s", nfp.loads[i].name, nfp.loads[i].ms, nfp.loads[i].found ? "" : " MISSING" );
	if( nfp.numloads > NFP_MAX_LOADS )
		Con_Printf( " (+%d)", nfp.numloads - NFP_MAX_LOADS );
	Con_Printf( "\n" );
}

// the interval of a frame is only known when the next one starts
static void NF_ProfFinishPrev( double now )
{
	nfp_frame_t *f = &nfp.cur;

	if( !nfp.have_prev )
		return;

	nfp.have_prev = false;
	f->interval = ( now - nfp.frame_start ) * 1000.0;

	// a screenshot (test scenarios) takes ~250 ms, not a spike of the game
	if( f->sections[NFP_SHOT] > 0.0f )
	{
		nfp.numshots++;
		return;
	}

	nfp.history[nfp.numhistory % NFP_HISTORY] = *f;
	nfp.numhistory++;

	if( f->interval >= nf_prof_spike.value && ( f->state != NFP_STATE_OTHER || nf_prof.value >= 2 ))
		NF_ProfPrintFrame( f );
}

void NF_ProfFrameBegin( int state )
{
	double now;

	if( !nf_prof.value )
	{
		nfp.have_prev = nfp.inframe = false;
		return;
	}

	now = Platform_DoubleTime();
	NF_ProfFinishPrev( now );

	memset( &nfp.cur, 0, sizeof( nfp.cur ));
	nfp.cur.state = state;
	nfp.numloads = 0;
	nfp.loadms = 0.0f;
	nfp.frame_start = now;
	nfp.inframe = true;
	nfp.framenum++;
}

void NF_ProfFrameEnd( void )
{
	if( !nf_prof.value || !nfp.inframe )
		return;

	nfp.frame_end = Platform_DoubleTime();
	nfp.cur.work = ( nfp.frame_end - nfp.frame_start ) * 1000.0;
	nfp.inframe = false;
	nfp.have_prev = true;
}

void NF_ProfSectionBegin( nfp_section_t s )
{
	nfp.section_start[s] = Platform_DoubleTime();
}

void NF_ProfSectionEnd( nfp_section_t s )
{
	if( !nfp.inframe || nfp.section_start[s] == 0.0 )
		return;

	nfp.cur.sections[s] += ( Platform_DoubleTime() - nfp.section_start[s] ) * 1000.0;
	nfp.section_start[s] = 0.0;
}

void NF_ProfFileLoad( const char *name, double start, qboolean found )
{
	float ms;

	if( !nfp.inframe )
		return;

	ms = ( Platform_DoubleTime() - start ) * 1000.0;

	if( nfp.numloads < NFP_MAX_LOADS )
	{
		nfp_load_t *l = &nfp.loads[nfp.numloads];

		Q_strncpy( l->name, name, sizeof( l->name ));
		l->ms = ms;
		l->found = found;
	}

	nfp.numloads++;
	nfp.loadms += ms;

	if( nf_prof.value >= 2 && nfp.cur.state != NFP_STATE_OTHER )
		Con_Printf( "nf_prof: load %s %.2f ms%s\n", name, ms, found ? "" : " MISSING" );
}

void NF_ProfMovieFrames( int decoded, qboolean shown )
{
	double now;

	if( decoded > 1 )
		nfp.mv_skipped += decoded - 1;

	if( !shown )
		return;

	now = Platform_DoubleTime();

	if( nfp.mv_last > 0.0 && now - nfp.mv_last < 1.0 ) // not across movies
	{
		float gap = ( now - nfp.mv_last ) * 1000.0;

		nfp.mv_gapsum += gap;
		nfp.mv_gapmax = Q_max( nfp.mv_gapmax, gap );
		nfp.mv_gaps++;
		if( gap > 50.0f )
			nfp.mv_late++;
	}

	nfp.mv_last = now;
	nfp.mv_shown++;
}

static int NF_ProfSortFloat( const void *a, const void *b )
{
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? -1 : x > y;
}

static void NF_ProfReportState( char state, const char *label )
{
	static float values[NFP_HISTORY];
	float sum[NFP_COUNT] = { 0 }, max[NFP_COUNT] = { 0 };
	float worksum = 0.0f, workmax = 0.0f, total = 0.0f;
	int n = 0, spikes = 0, i, j;
	int count = Q_min( nfp.numhistory, NFP_HISTORY );

	for( i = 0; i < count; i++ )
	{
		const nfp_frame_t *f = &nfp.history[i];

		if( f->state != state )
			continue;

		values[n++] = f->interval;
		total += f->interval;
		worksum += f->work;
		workmax = Q_max( workmax, f->work );
		if( f->interval >= nf_prof_spike.value )
			spikes++;

		for( j = 0; j < NFP_COUNT; j++ )
		{
			sum[j] += f->sections[j];
			max[j] = Q_max( max[j], f->sections[j] );
		}
	}

	if( !n )
	{
		Con_Printf( "nf_prof: %s: no frames\n", label );
		return;
	}

	qsort( values, n, sizeof( values[0] ), NF_ProfSortFloat );

	Con_Printf( "nf_prof: %s: %d frames, %.1f fps, frame avg %.2f p95 %.2f p99 %.2f max %.2f ms, %d over %.0f ms\n",
		label, n, n * 1000.0f / total, total / n, values[n * 95 / 100], values[n * 99 / 100], values[n - 1],
		spikes, nf_prof_spike.value );
	Con_Printf( "nf_prof: %s: work avg %.2f max %.2f ms\n", label, worksum / n, workmax );

	for( j = 0; j < NFP_COUNT; j++ )
	{
		if( max[j] > 0.0f )
			Con_Printf( "nf_prof: %s:   %-6s avg %6.2f max %6.2f ms\n", label, nfp_names[j], sum[j] / n, max[j] );
	}
}

static void NF_ProfReport_f( void )
{
	if( nfp.numhistory > NFP_HISTORY )
		Con_Printf( "nf_prof: last %d of %d frames\n", NFP_HISTORY, nfp.numhistory );

	if( nfp.numshots )
		Con_Printf( "nf_prof: %d screenshot frames left out\n", nfp.numshots );

	NF_ProfReportState( NFP_STATE_GAME, "game" );
	NF_ProfReportState( NFP_STATE_MOVIE, "movie" );

	if( nfp.mv_shown )
	{
		Con_Printf( "nf_prof: movie video: %d frames shown, %d skipped, gap avg %.1f max %.1f ms, %d gaps over 50 ms\n",
			nfp.mv_shown, nfp.mv_skipped, nfp.mv_gaps ? nfp.mv_gapsum / nfp.mv_gaps : 0.0f, nfp.mv_gapmax, nfp.mv_late );
	}
}

static void NF_ProfReset_f( void )
{
	nfp.numhistory = nfp.numshots = 0;
	nfp.mv_shown = nfp.mv_skipped = nfp.mv_gaps = nfp.mv_late = 0;
	nfp.mv_gapsum = nfp.mv_gapmax = 0.0f;
	nfp.mv_last = 0.0;
	Con_Printf( "nf_prof: history cleared\n" );
}

void NF_ProfInit( void )
{
	Cvar_RegisterVariable( &nf_prof );
	Cvar_RegisterVariable( &nf_prof_spike );
	Cmd_AddCommand( "nf_prof_report", NF_ProfReport_f, "nf_prof: frame time statistics of the recorded frames" );
	Cmd_AddCommand( "nf_prof_reset", NF_ProfReset_f, "nf_prof: clear the recorded frames" );
}
