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

typedef struct
{
	model_t *model;
	float   zoffset;	// height key / 10 (camera z offset)
	vec2_t  scroll;		// texture scroll speed (u, v) per second
} nf_skylayer_t;

static struct
{
	model_t      *dome;
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
static void R_DrawNightfireSkyLayer( model_t *m, const vec3_t origin, int rendermode, const vec2_t scroll )
{
	cl_entity_t *e = &nfworld.ent;
	qboolean doscr = ( scroll != NULL && ( scroll[0] != 0.0f || scroll[1] != 0.0f ));

	memset( e, 0, sizeof( *e ));
	e->model = m;
	e->curstate.modelindex = 0;
	e->curstate.rendermode = rendermode;
	e->curstate.renderamt = 255;
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
R_DrawNightfireSky

draw the sky models (dome around the camera, terrain/ocean at the world
origin, cloud layers around the camera at their height), then clear depth
so the world always covers them
=============
*/
void R_DrawNightfireSky( void )
{
	vec3_t org;

	if( !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	if( !nfworld.dome && !nfworld.cloudlow.model && !nfworld.cloudhigh.model &&
		!nfworld.terrain.model && !nfworld.ocean.model )
		return;

	R_AllowFog( false );

	// painter order far to near; depth is cleared after the sky anyway
	if( nfworld.dome )
		R_DrawNightfireSkyLayer( nfworld.dome, RI.rvp.vieworigin, kRenderNormal, NULL );

	if( nfworld.terrain.model )
		R_DrawNightfireSkyLayer( nfworld.terrain.model, vec3_origin, kRenderTransAlpha, NULL );

	if( nfworld.ocean.model )
		R_DrawNightfireSkyLayer( nfworld.ocean.model, vec3_origin, kRenderTransAlpha, NULL );

	if( nfworld.cloudhigh.model )
	{
		VectorCopy( RI.rvp.vieworigin, org );
		org[2] += nfworld.cloudhigh.zoffset;
		R_DrawNightfireSkyLayer( nfworld.cloudhigh.model, org, kRenderTransAlpha, nfworld.cloudhigh.scroll );
	}

	if( nfworld.cloudlow.model )
	{
		VectorCopy( RI.rvp.vieworigin, org );
		org[2] += nfworld.cloudlow.zoffset;
		R_DrawNightfireSkyLayer( nfworld.cloudlow.model, org, kRenderTransAlpha, nfworld.cloudlow.scroll );
	}

	R_AllowFog( true );

	R_LoadIdentity();
	pglClear( GL_DEPTH_BUFFER_BIT );
}
