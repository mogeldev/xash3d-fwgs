/* Synthetic dynamic-light regressions: no retail map or renderer required. */
#include "common.h"
#if XASH_ENGINE_TESTS
#include "xash3d_mathlib.h"
#include "com_model.h"
#include "q_client.h"
#include "tests.h"
#include "../../ref/common/ref_dlight.h"

void Test_RunDlightMarking( void )
{
	model_t model = { 0 };
	mnode_t nodes[3] = { 0 };
	mplane_t split = { 0 }, plane = { 0 };
	msurface_t surfaces[3] = { 0 };
	mextrasurf_t info[3] = { 0 };
	dlight_t light = { 0 };

	model.nodes = nodes;
	model.surfaces = surfaces;
	model.nummodelsurfaces = 2;
	nodes[0].plane = &split;
	nodes[0].numsurfaces_0 = 2;
	nodes[0].children_[0] = &nodes[1];
	nodes[0].children_[1] = &nodes[2];
	nodes[1].contents = nodes[2].contents = CONTENTS_EMPTY;
	split.normal[0] = 1;
	plane.normal[2] = 1;
	plane.type = 2;
	light.radius = 110;
	VectorSet( light.origin, 0, 0, 2 );
	for( int i = 0; i < 3; i++ )
	{
		surfaces[i].plane = &plane;
		surfaces[i].info = &info[i];
		info[i].lmvecs[0][0] = info[i].lmvecs[1][1] = 1;
		info[i].lightmapmins[0] = info[i].lightmapmins[1] = -16;
		info[i].lightextents[0] = info[i].lightextents[1] = 32;
	}
	info[1].lightmapmins[0] = 1000;

	// Both sides of both node-distance gates must retain the nearby LCA face.
	const float distances[] = { -1000, -200, 200, 1000 };
	for( int i = 0; i < (int)ARRAYSIZE( distances ); i++ )
	{
		split.dist = distances[i];
		R_MarkModelLights( &light, 1, &model, i + 1, 3, true, false );
		TASSERT_EQi( surfaces[0].dlightframe, i + 1 );
		TASSERT_EQi( surfaces[0].dlightbits, 1 );
		TASSERT_EQi( surfaces[1].dlightframe, 0 );
		R_MarkModelLights( &light, 2, &model, i + 1, 3, true, false );
		TASSERT_EQi( surfaces[0].dlightbits, 3 );
		R_MarkModelLights( &light, 4, &model, 50, 3, false, false );
		TASSERT_EQi( surfaces[0].dlightframe, i + 1 );
	}

	// A face in a pruned descendant can cross its ancestor split plane too.
	nodes[0].numsurfaces_0 = 0;
	nodes[1].contents = 0;
	nodes[1].plane = &plane;
	nodes[1].numsurfaces_0 = 2;
	nodes[1].children_[0] = nodes[1].children_[1] = &nodes[2];
	split.dist = 1000;
	R_MarkModelLights( &light, 1, &model, 55, 3, true, false );
	TASSERT_EQi( surfaces[0].dlightframe, 55 );
	nodes[1].contents = CONTENTS_EMPTY;
	nodes[0].numsurfaces_0 = 2;

	// Coplanar GoldSrc retains the existing frame/bit behavior.
	nodes[0].plane = &plane;
	R_MarkModelLights( &light, 8, &model, 60, 3, false, false );
	TASSERT_EQi( surfaces[0].dlightframe, 60 );
	TASSERT_EQi( surfaces[0].dlightbits, 8 );
	R_MarkModelLights( &light, 16, &model, 60, 3, false, false );
	TASSERT_EQi( surfaces[0].dlightbits, 24 );

	// Actual surface distance rejects a remote face even on a nearby node.
	nodes[0].plane = &split;
	split.dist = 0;
	plane.dist = 200;
	R_MarkModelLights( &light, 1, &model, 61, 3, true, false );
	TASSERT_EQi( surfaces[0].dlightframe, 60 );
	plane.dist = 0;

	// A flipped plane must project onto the same face.
	plane.normal[2] = -1;
	plane.type = 5;
	R_MarkModelLights( &light, 2, &model, 62, 3, true, false );
	TASSERT_EQi( surfaces[0].dlightframe, 62 );

	// Oblique plane projection must use its own distance, not the owner's.
	VectorSet( plane.normal, 0.6f, 0, 0.8f );
	plane.type = 5;
	VectorSet( light.origin, 12, 0, 16 );
	info[0].lightextents[0] = info[0].lightextents[1] = 0;
	info[0].lightmapmins[0] = info[0].lightmapmins[1] = 0;
	light.radius = 22;
	R_MarkModelLights( &light, 2, &model, 64, 3, true, false );
	TASSERT_EQi( surfaces[0].dlightframe, 64 );
	VectorSet( plane.normal, 0, 0, -1 );
	VectorSet( light.origin, 0, 0, 2 );
	light.radius = 110;

	// Brush-only face range: no nodes required, no world/other brush pollution.
	model.nodes = NULL;
	model.firstmodelsurface = 2;
	model.nummodelsurfaces = 1;
	R_MarkModelLights( &light, 4, &model, 63, 3, true, true );
	TASSERT_EQi( surfaces[2].dlightframe, 63 );
	TASSERT_EQi( surfaces[2].dlightbits, 4 );
	TASSERT_EQi( surfaces[0].dlightframe, 64 );
	TASSERT_EQi( surfaces[1].dlightframe, 0 );
}
#endif
