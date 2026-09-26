// ATOM S3-DRAW — THE ID PASS'S FRAGMENT STAGE: the AtomId words, nothing else. No
// material, no texture (alpha-tested items never reach this pass: the split keeps
// them on PBS).
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
	uvec2 pad;
} pc;

layout( location = 0 ) flat in uint inSlot;
layout( location = 1 ) flat in uint inTriBase;

layout( location = 0 ) out uvec2 outIds;

void main()
{
	uint tri = inTriBase + uint( gl_PrimitiveID );
	AtomWordRef words = AtomWordRef( pc.triWords );
	outIds = uvec2( jahAtomIdPackX( inSlot, words.v[tri * 2u + 1u] ), words.v[tri * 2u] );
}
