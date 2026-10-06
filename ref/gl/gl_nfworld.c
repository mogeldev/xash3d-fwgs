/*
gl_nfworld.c - James Bond 007: Nightfire (PC) world settings: sky dome and
lightmap overbright.
*/

// Read on R_NewMap from the worldspawn entity of a Nightfire map:
//
// * "skydome": Nightfire maps have no sky geometry (their "special/sky"
//   surfaces carry no polygons); the sky is a studio model,
//   models/sky/<name>.mdl, converted from MDLZ like any other model. It is
//   drawn like a skybox: centred on the camera, before the world, and the
//   depth buffer is cleared afterwards so the world always covers it. The
//   cloud layers ("skycloudlow" / "skycloudhigh" with their _height and _speed
//   keys) are not drawn: how the retail engine places them is not known.
//
// * "overbright": the retail client sets its r_overbright cvar from this key
//   ("Set overbright amount 0-255, 128 = off"), so the lightmap brightness
//   factor is overbright / 128 (e.g. 192 -> 1.5). The GL renderer draws
//   lightmaps with a 2x modulate scaled by a constant (128/192 by default,
//   i.e. 1.33); for Nightfire maps that constant becomes overbright / 256.

#include "gl_local.h"

static struct
{
	model_t     *dome;
	cl_entity_t ent;
	float       overbright;	// worldspawn "overbright", 0 if not set
} nfworld;

/*
=============
R_NightfireNewMap

read the Nightfire worldspawn keys of the new map
=============
*/
void R_NightfireNewMap( void )
{
	char token[1024], key[1024], path[MAX_QPATH];
	char *data;

	memset( &nfworld, 0, sizeof( nfworld ));

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

		if( !Q_stricmp( key, "skydome" ) && token[0] )
		{
			Q_snprintf( path, sizeof( path ), "models/sky/%s.mdl", token );
			nfworld.dome = gEngfuncs.Mod_ForName( path, false, false );

			if( !nfworld.dome || nfworld.dome->type != mod_studio )
			{
				gEngfuncs.Con_Reportf( S_WARN "%s: can't load sky dome %s\n", __func__, path );
				nfworld.dome = NULL;
			}
			else gEngfuncs.Con_Reportf( "Nightfire sky dome: %s\n", path );
		}
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
R_DrawNightfireSky

draw the sky dome around the camera, then clear depth
=============
*/
void R_DrawNightfireSky( void )
{
	cl_entity_t *e = &nfworld.ent;

	if( !nfworld.dome || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	e->model = nfworld.dome;
	e->curstate.modelindex = 0;
	e->curstate.rendermode = kRenderNormal;
	e->curstate.renderamt = 255;
	e->curstate.framerate = 1.0f;
	VectorCopy( RI.rvp.vieworigin, e->origin );
	VectorCopy( RI.rvp.vieworigin, e->curstate.origin );
	VectorClear( e->angles );
	VectorClear( e->curstate.angles );

	R_AllowFog( false );
	R_DrawStudioModelBuiltin( e );
	R_AllowFog( true );

	R_LoadIdentity();
	pglClear( GL_DEPTH_BUFFER_BIT );
}
