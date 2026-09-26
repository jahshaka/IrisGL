// ATOM S3-DRAW — THE ID PASS'S VERTEX STAGE (OgreAtomIdPass.cpp records it).
//
// No vertex attributes: every vertex is PULLED through the GPU scene's tables, the
// same rows the decode and the voxeliser read (jah_geom_rows.glsl, the ONE copy of
// the geometry-row decode). The draw is one VkDrawIndexedIndirectCommand per
// surviving instance, written by the cull's CUT (ATOM-CLUSTER-CUT, JahCullCut_cs.glsl):
// firstInstance is the INSTANCE SLOT and [firstIndex, +indexCount) the instance's
// run of the view's COMPACTED STREAM (JahCullEmit_cs.glsl), which holds REAL,
// mesh-local vertex indices — so gl_VertexIndex IS the vertex, the post-transform
// cache sees the reuse, and the position is read through the mesh's CLUSTER ROW
// (GpuMesh::dag.w: level 0's vertices; every cluster of every depth indexes them).
//
// The transform is the decode's own order — world rows, then the view-projection
// rows — so the triangle the decode rebuilds from the id lands where this one did.
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_GOOGLE_include_directive : require

#include "jah_geom_rows.glsl"

layout( buffer_reference, std430, buffer_reference_align = 16 ) readonly buffer AtomVec4Ref
{
	uvec4 v[];
};
layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer AtomWordRef
{
	uint v[];
};

layout( push_constant ) uniform AtomIdPc
{
	vec4  viewProjRow[4];   // row i of proj * view, the pass's own (texture flip included)
	uvec2 instances;        // GpuScene::instanceBuffer: 10 uvec4 per GpuInstance
	uvec2 meshes;           // GpuScene::meshBuffer: 4 uvec4 per GpuMesh (dag = lane 3)
	uvec2 rows;             // GpuScene::geomBuffer: 3 uvec4 per geometry row
	uvec2 triWords;         // GpuCull::cutTriWords: two words per stream triangle (the fragment's)
	uvec2 slotBase;         // GpuCull::cutSlotBase: the slot's first stream triangle
	uvec2 pad;
} pc;

layout( location = 0 ) flat out uint outSlot;
layout( location = 1 ) flat out uint outTriBase;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main()
{
	uint slot = uint( gl_InstanceIndex );
	AtomVec4Ref inst = AtomVec4Ref( pc.instances );
	uint mesh = inst.v[slot * 10u + 6u].w;
	uint rowIndex = AtomVec4Ref( pc.meshes ).v[mesh * 4u + 3u].w;

	AtomVec4Ref rows = AtomVec4Ref( pc.rows );
	GeometryRow row;
	row.addresses = rows.v[rowIndex * 3u];
	row.layout0 = rows.v[rowIndex * 3u + 1u];
	row.layout1 = rows.v[rowIndex * 3u + 2u];

	vec4 p = vec4( geomPosition( row, uint( gl_VertexIndex ) ), 1.0 );
	vec3 w = vec3( dot( uintBitsToFloat( inst.v[slot * 10u + 0u] ), p ),
				   dot( uintBitsToFloat( inst.v[slot * 10u + 1u] ), p ),
				   dot( uintBitsToFloat( inst.v[slot * 10u + 2u] ), p ) );
	vec4 w4 = vec4( w, 1.0 );
	gl_Position = vec4( dot( pc.viewProjRow[0], w4 ), dot( pc.viewProjRow[1], w4 ),
						dot( pc.viewProjRow[2], w4 ), dot( pc.viewProjRow[3], w4 ) );

	outSlot = slot;
	outTriBase = AtomWordRef( pc.slotBase ).v[slot];
}
