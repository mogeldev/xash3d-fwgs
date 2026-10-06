/*
gl_nfsky.c - James Bond 007: Nightfire (PC) sky dome.
*/

// Nightfire maps have no sky geometry: their "special/sky" surfaces carry no
// polygons. The sky is a studio model named by the worldspawn key "skydome"
// (models/sky/<name>.mdl, converted from MDLZ like any other model). It is
// drawn here like a skybox: centred on the camera, before the world, and the
// depth buffer is cleared afterwards so the world always covers it.
//
// The cloud layers ("skycloudlow" / "skycloudhigh" with their _height and
// _speed keys) are not drawn yet; how the retail engine places them is not
// known.

#include "gl_local.h"

static struct
{
	model_t     *dome;
	cl_entity_t ent;
} nfsky;

/*
=============
R_NightfireSkyNewMap

look up the worldspawn "skydome" key and load its model
=============
*/
void R_NightfireSkyNewMap( void )
{
	char token[1024], key[1024], path[MAX_QPATH];
	char *data;

	memset( &nfsky, 0, sizeof( nfsky ));

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
			nfsky.dome = gEngfuncs.Mod_ForName( path, false, false );

			if( !nfsky.dome || nfsky.dome->type != mod_studio )
			{
				gEngfuncs.Con_Reportf( S_WARN "%s: can't load sky dome %s\n", __func__, path );
				nfsky.dome = NULL;
			}
			else gEngfuncs.Con_Reportf( "Nightfire sky dome: %s\n", path );
		}
	}
}

/*
=============
R_DrawNightfireSky

draw the sky dome around the camera, then clear depth
=============
*/
void R_DrawNightfireSky( void )
{
	cl_entity_t *e = &nfsky.ent;

	if( !nfsky.dome || !FBitSet( RI.rvp.flags, RF_DRAW_WORLD ))
		return;

	e->model = nfsky.dome;
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
