// ATOM S3-DRAW — THE ID PASS'S FRAGMENT STAGE: the AtomId words, nothing else. No
// material, no texture — except a CUT-OUT's mask (JAH_MASKED, ENGINE-POLISH-ATOM-1
// stage 3: the masked ranges' pipelines): its fragment is discarded where the
// material's mask is clear (jah_alpha_mask.glsl) at the pyramid level the fragment's
// UV footprint asks for (ATOM-3 stage 2), so the ids, the depth and the
// depth pyramid built from it are holed by construction, and the decode never
// re-tests alpha (its twin drops the alpha test: a re-test at another sampling
// would hole the picture where the id pass wrote).
//
// THE TRIANGLE'S WORDS (ATOM-CLUSTER-CUT): the cut's emit job wrote two words per
// triangle of the view's compacted stream — (cluster << 8 | triangle in the cluster)
// and the cluster's DAG depth; this triangle is the instance's first stream
// triangle (the vertex stage's flat) plus gl_PrimitiveID, which counts the draw's
// triangles from its first index whether or not the rasteriser culled any.
// jah_atom_id.glsl packs them (the decode unpacks with the same file).
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_GOOGLE_include_directive : require

#include "jah_atom_id.glsl"
#ifdef JAH_MASKED
#include "jah_alpha_mask.glsl"
#endif

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer AtomWordRef
{
	uint v[];
};

layout( push_constant ) uniform AtomIdPc
{
	vec4  viewProjRow[4];
	uvec2 instances;
	uvec2 meshes;
	uvec2 rows;
	uvec2 triWords;
	uvec2 slotBase;
	uvec2 maskTable;
} pc;

layout( location = 0 ) flat in uint inSlot;
layout( location = 1 ) flat in uint inTriBase;
#ifdef JAH_MASKED
layout( location = 2 ) in vec2 inUv;
#endif

layout( location = 0 ) out uvec2 outIds;

void main()
{
#ifdef JAH_MASKED
	// THE FOOTPRINT FIRST, in uniform control flow (ATOM-3 stage 2: the pyramid level it asks for).
	const vec2 duvdx = dFdx( inUv ), duvdy = dFdy( inUv );
	// submesh 0's record: an Atom item has one submesh (OgreScene::atomRowReadable)
	const uint rec = jahMaskRecord( pc.maskTable, inSlot, 0u );
	if( rec != kJahMaskNone && !jahMaskSolidAtFootprint( pc.maskTable, rec, inUv, duvdx, duvdy ) )
		discard;
#endif
	uint tri = inTriBase + uint( gl_PrimitiveID );
	AtomWordRef words = AtomWordRef( pc.triWords );
	outIds = uvec2( jahAtomIdPackX( inSlot, words.v[tri * 2u + 1u] ), words.v[tri * 2u] );
}
