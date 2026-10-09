/*
gl_nfworld.c - James Bond 007: Nightfire (PC) world settings: sky models and
lightmap overbright.
*/

// Read on R_NewMap from the worldspawn entity of a Nightfire map:
//
// * "skydome": Nightfire maps have no sky geometry (their "special/sky"
//   surfaces carry no polygons); the sky is a studio model,
//   models/sky/<name>.mdl, converted from MDLZ like any other model. It is
//   drawn like a skybox: centred on the camera, before the world, and the
//   depth buffer is cleared afterwards so the world always covers it.
//
// * "skycloudlow" / "skycloudhigh": two more sky models (cloud layers),
//   also centred on the camera but offset up/down by their
//   "skycloud*_height" key divided by 10 (retail client.dll: the value lands
//   in cvars cl_skylow_height / cl_skyhigh_height, defaults 1000 / 2000, and
//   the render code adds cvar/10 to the camera z). Their textures have an
//   alpha channel (soft clouds), so they blend over the dome, and they
//   scroll: "skycloud*_speed" "u v" (retail cvars cl_sky*_speed_s/t,
//   defaults 0.01923 0.001923 / 0.027027 0.0027027) scrolls the texture
//   matrix. All sky models are drawn with yaw 270 like the retail client
//   (the retail dome "rotation" cvar cl_skydome_rotation 0.015 is stored
//   into an int angle every frame and truncates back to 270, so the retail
//   dome does not actually rotate).
//
// * "skyterrain" / "skyocean": two more sky models drawn at the world
//   origin (not camera-centred): distant terrain rings / the ocean plane.
//
// * "skycloudlow_lightning" / "skycloudhigh_lightning" (int, cvars
//   cl_skylow_lightning / cl_skyhigh_lightning, default 0) and "skylightning"
//   (cl_lightning_enable, default 1): lightning flashes on the cloud layers.
//   Retail client.dll (CloudParameters update, 0x41001490) runs a per-frame
//   state machine per cloud submesh: while idle, a flash starts when
//   RandomLong( 0, lightning ) < 5; it lasts RandomLong( cl_lightning_min 3,
//   cl_lightning_max 35 ) frames, and moves the material's Light1 (a point
//   light) to the cloud origin + a random offset (+-1000, +-1000, +-50) that
//   jitters by random / framesLeft per frame and ends in a division by zero
//   (light at infinity = one dark frame). The shader that turns Light1 into
//   cloud brightness is not known: this port approximates it by an additive
//   second pass of the layer whose strength follows the light attenuation
//   (retail default Light1Attenuation 0, 0.01, 0 = 1 / (0.01 * distance)).
//   The retail update runs once per rendered frame; here it runs at a fixed
//   60 Hz so the flash timing does not depend on the frame rate.
//
// * "overbright": the retail client sets its r_overbright cvar from this key
//   ("Set overbright amount 0-255, 128 = off"), so the lightmap brightness
//   factor is overbright / 128 (e.g. 192 -> 1.5). The GL renderer draws
//   lightmaps with a 2x modulate scaled by a constant (128/192 by default,
//   i.e. 1.33); for Nightfire maps that constant becomes overbright / 256.

#include "gl_local.h"

// retail defaults (client.dll cvars), used when a map sets the model but not
// the key
#define NF_SKY_LOW_HEIGHT    1000.0f
#define NF_SKY_HIGH_HEIGHT   2000.0f
#define NF_SKY_LOW_SPEED_S   0.01923f
#define NF_SKY_LOW_SPEED_T   0.001923f
#define NF_SKY_HIGH_SPEED_S  0.027027f
#define NF_SKY_HIGH_SPEED_T  0.0027027f

#define NF_SKY_YAW  270.0f	// retail draws every sky model with yaw 270

// retail lightning cvars (client.dll): flash length in frames, start chance
#define NF_LIGHTNING_MIN_FRAMES  3
#define NF_LIGHTNING_MAX_FRAMES  35
#define NF_LIGHTNING_START_ROLL  5
#define NF_LIGHTNING_TICK_RATE   60.0
#define NF_LIGHTNING_MAX_TICKS   8
// [assumed] brightness gain of the additive flash pass
#define NF_LIGHTNING_GAIN        3.0f

typedef struct
{
	model_t *model;
	float   zoffset;	// height key / 10 (camera z offset)
	vec2_t  scroll;		// texture scroll speed (u, v) per second
	int     lightning;	// "skycloud*_lightning": 0 = none, else start-roll range
	int     frames;		// flash frames left, 0 = idle
	qboolean flash;		// retail "A8": the Light1 point light is on
	qboolean logged;
	vec3_t  loffset;	// light offset from the layer origin
} nf_skylayer_t;

static struct
{
	model_t      *dome;
	qboolean     lightning;		// worldspawn "skylightning" (cl_lightning_enable)
	double       lightningtime;	// time of the last lightning tick
	nf_skylayer_t cloudlow;
	nf_skylayer_t cloudhigh;
	nf_skylayer_t terrain;
	nf_skylayer_t ocean;
	cl_entity_t  ent;
	float        overbright;	// worldspawn "overbright", 0 if not set
} nfworld;

/*
=============
R_NightfireSkyModel

load one models/sky/<name>.mdl layer
=============
*/
static model_t *R_NightfireSkyModel( const char *kind, const char *name )
{
	char path[MAX_QPATH];
	model_t *m;

	Q_snprintf( path, sizeof( path ), "models/sky/%s.mdl", name );
	m = gEngfuncs.Mod_ForName( path, false, false );

	if( !m || m->type != mod_studio )
	{
		gEngfuncs.Con_Reportf( S_WARN "%s: can't load %s %s\n", __func__, kind, path );
		return NULL;
	}

	gEngfuncs.Con_Reportf( "Nightfire sky %s: %s\n", kind, path );
	return m;
}

/*
=============
R_NightfireSkySpeed

parse a "u v" scroll speed value
=============
*/
static void R_NightfireSkySpeed( const char *value, vec2_t scroll )
{
	char buf[1024], tok[64];
	char *data;

	Q_strncpy( buf, value, sizeof( buf ));
	data = buf;

	data = COM_ParseFile( data, tok, sizeof( tok ));
	if( !data )
		return;
	scroll[0] = Q_atof( tok );

	data = COM_ParseFile( data, tok, sizeof( tok ));
	if( !data )
		return;
	scroll[1] = Q_atof( tok );
}

/*
=============
R_NightfireNewMap

read the Nightfire worldspawn keys of the new map
=============
*/
void R_NightfireNewMap( void )
{
	char token[1024], key[1024];
	char *data;

	memset( &nfworld, 0, sizeof( nfworld ));

	// retail cvar defaults
	nfworld.lightning = true;
	nfworld.lightningtime = gp_cl->time;
	nfworld.cloudlow.zoffset = NF_SKY_LOW_HEIGHT / 10.0f;
	nfworld.cloudlow.scroll[0] = NF_SKY_LOW_SPEED_S;
	nfworld.cloudlow.scroll[1] = NF_SKY_LOW_SPEED_T;
	nfworld.cloudhigh.zoffset = NF_SKY_HIGH_HEIGHT / 10.0f;
	nfworld.cloudhigh.scroll[0] = NF_SKY_HIGH_SPEED_S;
	nfworld.cloudhigh.scroll[1] = NF_SKY_HIGH_SPEED_T;

	if( !WORLDMODEL || !WORLDMODEL->entities )
		return;

	data = WORLDMODEL->entities;

	// the first entity block is worldspawn
	data = COM_ParseFile( data, token, sizeof( token ));
	if( !data || token[0] != '{' )
		return;

	while( 1 )
	{
		data = COM_ParseFile( data, key, sizeof( key ));
		if( !data || key[0] == '}' )
			break;

		data = COM_ParseFile( data, token, sizeof( token ));
		if( !data )
			break;

		if( !token[0] )
			continue;

		if( !Q_stricmp( key, "skydome" ))
			nfworld.dome = R_NightfireSkyModel( "dome", token );
		else if( !Q_stricmp( key, "skycloudlow" ))
		{
			if( Q_stricmp( token, "none" ))	// dm_japan disables the layer this way
				nfworld.cloudlow.model = R_NightfireSkyModel( "cloudlow", token );
		}
		else if( !Q_stricmp( key, "skycloudhigh" ))
		{
			if( Q_stricmp( token, "none" ))
				nfworld.cloudhigh.model = R_NightfireSkyModel( "cloudhigh", token );
		}
		else if( !Q_stricmp( key, "skyterrain" ))
		{
			if( Q_stricmp( token, "none" ))
				nfworld.terrain.model = R_NightfireSkyModel( "terrain", token );
		}
		else if( !Q_stricmp( key, "skyocean" ))
		{
			if( Q_stricmp( token, "none" ))
				nfworld.ocean.model = R_NightfireSkyModel( "ocean", token );
		}
		else if( !Q_stricmp( key, "skycloudlow_height" ))
			nfworld.cloudlow.zoffset = Q_atof( token ) / 10.0f;
		else if( !Q_stricmp( key, "skycloudhigh_height" ))
			nfworld.cloudhigh.zoffset = Q_atof( token ) / 10.0f;
		else if( !Q_stricmp( key, "skycloudlow_speed" ))
			R_NightfireSkySpeed( token, nfworld.cloudlow.scroll );
		else if( !Q_stricmp( key, "skycloudhigh_speed" ))
			R_NightfireSkySpeed( token, nfworld.cloudhigh.scroll );
		else if( !Q_stricmp( key, "skycloudlow_lightning" ))
			nfworld.cloudlow.lightning = Q_atoi( token );
		else if( !Q_stricmp( key, "skycloudhigh_lightning" ))
			nfworld.cloudhigh.lightning = Q_atoi( token );
		else if( !Q_stricmp( key, "skylightning" ))
			nfworld.lightning = Q_atoi( token ) != 0;
		else if( !Q_stricmp( key, "overbright" ))
		{
			nfworld.overbright = bound( 0.0f, Q_atof( token ), 255.0f );
			gEngfuncs.Con_Reportf( "Nightfire lightmap overbright: %g (x%.2f)\n",
				nfworld.overbright, nfworld.overbright / 128.0f );
		}
	}
}

/*
=============
R_LightmapOverbrightScale

constant applied to the lightmap under the 2x overbright modulate
=============
*/
float R_LightmapOverbrightScale( void )
{
	if( nfworld.overbright > 0.0f )
		return nfworld.overbright / 256.0f;

	return 128.0f / 192.0f;
}

/*
=============
R_DrawNightfireSkyLayer

draw one sky model; the dome is opaque, the other layers blend with their
texture alpha (RGB textures come out opaque). Cloud layers scroll their
texture by their speed key.
=============
*/
static void R_DrawNightfireSkyLayer( model_t *m, const vec3_t origin, int rendermode, int renderamt, const vec2_t scroll )
{
	cl_entity_t *e = &nfworld.ent;
	qboolean doscr = ( scroll != NULL && ( scroll[0] != 0.0f || scroll[1] != 0.0f ));

	memset( e, 0, sizeof( *e ));
	e->model = m;
	e->curstate.modelindex = 0;
	e->curstate.rendermode = rendermode;
	e->curstate.renderamt = renderamt;
	e->curstate.framerate = 1.0f;
	VectorCopy( origin, e->origin );
	VectorCopy( origin, e->curstate.origin );
	e->angles[1] = NF_SKY_YAW;
	e->curstate.angles[1] = NF_SKY_YAW;

	if( doscr )
	{
		float u = scroll[0] * gp_cl->time;
		float v = scroll[1] * gp_cl->time;

		// wrap the phase so the offset stays exact in float
		u = (float)( u - floor( u ));
		v = (float)( v - floor( v ));
		pglMatrixMode( GL_TEXTURE );
		pglPushMatrix();
		pglTranslatef( u, v, 0.0f );
		pglMatrixMode( GL_MODELVIEW );
	}

	R_DrawStudioModelBuiltin( e );

	if( doscr )
	{
		pglMatrixMode( GL_TEXTURE );
		pglPopMatrix();
		pglMatrixMode( GL_MODELVIEW );
	}
}

/*
=============
R_NightfireLightningTick

one retail CloudParameters update (client.dll 0x41001490) for a cloud layer
=============
*/
static void R_NightfireLightningTick( nf_skylayer_t *l )
{
	if( !nfworld.lightning )
	{
		l->frames = 0;
		l->flash = false;
		return;
	}

	if( l->frames > 0 )
	{
		// running flash: the light drifts by random / framesLeft; retail
		// divides by zero on the last frame (light at infinity)
		if( --l->frames <= 0 )
		{
			l->flash = false;
			return;
		}

		l->loffset[0] += gEngfuncs.COM_RandomFloat( -100.0f, 100.0f ) / l->frames;
		l->loffset[1] += gEngfuncs.COM_RandomFloat( -100.0f, 100.0f ) / l->frames;
		l->loffset[2] += gEngfuncs.COM_RandomFloat( -25.0f, 25.0f ) / l->frames;
		return;
	}

	if( l->lightning <= 0 || gEngfuncs.COM_RandomLong( 0, l->lightning ) >= NF_LIGHTNING_START_ROLL )
	{
		l->flash = false;
		return;
	}

	l->frames = gEngfuncs.COM_RandomLong( NF_LIGHTNING_MIN_FRAMES, NF_LIGHTNING_MAX_FRAMES );
	l->loffset[0] = gEngfuncs.COM_RandomFloat( -1000.0f, 1000.0f );
	l->loffset[1] = gEngfuncs.COM_RandomFloat( -1000.0f, 1000.0f );
	l->loffset[2] = gEngfuncs.COM_RandomFloat( -50.0f, 50.0f );
	l->flash = true;

	if( !l->logged )
	{
		l->logged = true;
		gEngfuncs.Con_Reportf( "Nightfire sky lightning: first flash, %d frames\n", l->frames );
	}
}

/*
=============
R_NightfireLightningUpdate

advance the lightning state machines at a fixed tick rate
=============
*/
static void R_NightfireLightningUpdate( void )
{
	int ticks;

	if( !nfworld.cloudlow.lightning && !nfworld.cloudhigh.lightning )
		return;

	if( nfworld.lightningtime > gp_cl->time )
		nfworld.lightningtime = gp_cl->time;	// time went backwards (new map / demo)

	ticks = (int)(( gp_cl->time - nfworld.lightningtime ) * NF_LIGHTNING_TICK_RATE );
	if( ticks <= 0 )
		return;

	nfworld.lightningtime += ticks / NF_LIGHTNING_TICK_RATE;
	ticks = Q_min( ticks, NF_LIGHTNING_MAX_TICKS );

	while( ticks-- > 0 )
	{
		R_NightfireLightningTick( &nfworld.cloudlow );
		R_NightfireLightningTick( &nfworld.cloudhigh );
	}
}

/*
=============
R_DrawNightfireCloudLayer

draw a cloud layer; while its lightning flash is on, add a second additive
pass whose strength follows the Light1 attenuation 1 / (0.01 * distance)
=============
*/
static void R_DrawNightfireCloudLayer( nf_skylayer_t *l )
{
	vec3_t org;

	if( !l->model )
		return;

	VectorCopy( RI.rvp.vieworigin, org );
	org[2] += l->zoffset;
	R_DrawNightfireSkyLayer( l->model, org, kRenderTransAlpha, 255, l->scroll );

	if( l->flash )
	{
		float dist = Q_max( VectorLength( l->loffset ), 1.0f );
		float gain = bound( 0.0f, NF_LIGHTNING_GAIN * 100.0f / dist, 1.0f );

		if( gain > 0.01f )
			R_DrawNightfireSkyLayer( l->model, org, kRenderTransAdd, (int)( gain * 255.0f ), l->scroll );
	}
}

/*
=============
R_DrawNightfireSky

draw the sky models (dome around the camera, terrain/ocean at the world
origin, cloud layers around the camera at their height), then clear depth
so the world always covers them
=============
*/
void R_DrawNightfireSky( void )
{
	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	if( !nfworld.dome && !nfworld.cloudlow.model && !nfworld.cloudhigh.model &&
		!nfworld.terrain.model && !nfworld.ocean.model )
		return;

	R_NightfireLightningUpdate();

	R_AllowFog( false );

	// painter order far to near; depth is cleared after the sky anyway
	if( nfworld.dome )
	{
		R_DrawNightfireSkyLayer( nfworld.dome, RI.rvp.vieworigin, kRenderNormal, 255, NULL );
		// The camera-centred dome is nearer than the world-space terrain.
		// Keep its colour, but do not let its depth occlude the horizon.
		pglDepthMask( GL_TRUE );
		pglClear( GL_DEPTH_BUFFER_BIT );
	}

	if( nfworld.terrain.model )
		R_DrawNightfireSkyLayer( nfworld.terrain.model, vec3_origin, kRenderTransAlpha, 255, NULL );

	if( nfworld.ocean.model )
		R_DrawNightfireSkyLayer( nfworld.ocean.model, vec3_origin, kRenderTransAlpha, 255, NULL );

	R_DrawNightfireCloudLayer( &nfworld.cloudhigh );
	R_DrawNightfireCloudLayer( &nfworld.cloudlow );

	R_AllowFog( true );

	R_LoadIdentity();
	pglClear( GL_DEPTH_BUFFER_BIT );
}

