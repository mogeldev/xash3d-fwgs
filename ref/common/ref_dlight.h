/*
ref_dlight.h - dynamic-light marking shared with synthetic engine tests
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.
*/
#ifndef REF_DLIGHT_H
#define REF_DLIGHT_H

static void R_MarkLightSurfaceList( const dlight_t *light, int bit, model_t *model,
	int firstsurface, int numsurfaces, int framecount )
{
	const float maxdist = light->radius * light->radius;
	for( int i = 0; i < numsurfaces; i++ )
	{
		msurface_t *surf = &model->surfaces[firstsurface + i];
		const mextrasurf_t *info = surf->info;
		const float dist = PlaneDiff( light->origin, surf->plane );
		vec3_t impact;
		float s, t, l;

		if( dist * dist >= maxdist )
			continue;
		VectorMA( light->origin, -dist, surf->plane->normal, impact );
		l = DotProduct( impact, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		s = l - bound( 0, l + 0.5f, info->lightextents[0] );
		l = DotProduct( impact, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];
		t = l - bound( 0, l + 0.5f, info->lightextents[1] );
		if( s * s + t * t + dist * dist >= maxdist )
			continue;
		if( surf->dlightframe != framecount )
		{
			surf->dlightbits = bit;
			surf->dlightframe = framecount;
		}
		else surf->dlightbits |= bit;
	}
}

/*
=============
R_MarkLights
=============
*/
static void R_MarkLights( const dlight_t *light, int bit, const mnode_t *node, model_t *model, int dlightframecount, float radius_scale )
{
	const float virtual_radius = light->radius * Q_max( 1.0f, radius_scale );
	const float maxdist = light->radius * light->radius;
start:
	if( !node || node->contents < 0 )
		return;

	float dist = PlaneDiff( light->origin, node->plane );

	if( dist > virtual_radius )
	{
		node = node_child( node, 0, model );
		goto start;
	}

	if( dist < -virtual_radius )
	{
		node = node_child( node, 1, model );
		goto start;
	}

	const float dist_sq = dist * dist;

	// mark the polygons
	int firstsurface = node_firstsurface( node, model );
	int numsurfaces = node_numsurfaces( node, model );

	for( int i = 0; i < numsurfaces && dist_sq < maxdist; i++ )
	{
		vec3_t impact;
		float s, t, l;
		msurface_t *surf = &model->surfaces[firstsurface + i];
		const mextrasurf_t *info = surf->info;

		if( surf->plane->type < 3 )
		{
			VectorCopy( light->origin, impact );
			impact[surf->plane->type] -= dist;
		}
		else VectorMA( light->origin, -dist, surf->plane->normal, impact );

		// a1ba: the fix was taken from JoeQuake, which traces back to FitzQuake,
		// which attributes it to LadyHavoc (Darkplaces author)
		// clamp center of light to corner and check brightness
		l = DotProduct( impact, info->lmvecs[0] ) + info->lmvecs[0][3] - info->lightmapmins[0];
		s = l + 0.5;
		s = bound( 0, s, info->lightextents[0] );
		s = l - s;

		l = DotProduct( impact, info->lmvecs[1] ) + info->lmvecs[1][3] - info->lightmapmins[1];
		t = l + 0.5;
		t = bound( 0, t, info->lightextents[1] );
		t = l - t;

		if( s * s + t * t + dist_sq >= maxdist )
			continue;

		if( surf->dlightframe != dlightframecount )
		{
			surf->dlightbits = bit;
			surf->dlightframe = dlightframecount;
		}
		else surf->dlightbits |= bit;
	}

	R_MarkLights( light, bit, node_child( node, 0, model ), model, dlightframecount, radius_scale );
	node = node_child( node, 1, model );
	goto start;
}

static void R_MarkModelLights( const dlight_t *light, int bit, model_t *model,
	int framecount, float radius_scale, qboolean nightfire, qboolean brush )
{
	// Nightfire LCA ownership does not bound faces by ancestor split planes.
	// Scan the model range: this also handles brush models without render nodes.
	if( nightfire )
		R_MarkLightSurfaceList( light, bit, model, model->firstmodelsurface, model->nummodelsurfaces, framecount );
	else
		R_MarkLights( light, bit, model->nodes + ( brush ? model->hulls[0].firstclipnode : 0 ),
			model, framecount, radius_scale );
}

#endif /* REF_DLIGHT_H */
