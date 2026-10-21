// ATOM S3-DRAW — THE ID PASS'S VERTEX STAGE (OgreAtomIdPass.cpp records it).
//
// No vertex attributes: every vertex is PULLED through the GPU scene's tables, the
// same rows the decode and the voxeliser read (jah_geom_rows.glsl, the ONE copy of
// the geometry-row decode). The draw is one VkDrawIndexedIndirectCommand per
// surviving instance, written by the cull's CUT (ATOM-CLUSTER-CUT, JahCullCut_cs.glsl):
// firstInstance is the INSTANCE SLOT (or, for the late pass's per-cluster draws, an entry of their identity table)
// and [firstIndex, +indexCount) the instance's
// run of the view's COMPACTED STREAM (JahCullEmit_cs.glsl), which holds REAL,
// mesh-local vertex indices — so gl_VertexIndex IS the vertex, the post-transform
// cache sees the reuse, and the position is read through the mesh's CLUSTER ROW
// (GpuMesh::dag.w: level 0's vertices; every cluster of every depth indexes them).
//
// The transform is the decode's own order — world rows, then the view-projection
// rows — so the triangle the decode rebuilds from the id lands where this one did.
//
// JAH_MASKED (ENGINE-POLISH-ATOM-1 stage 3, the masked ranges): the vertex's texture
// coordinate rides to the fragment stage, which discards where the cut-out's mask is
// clear (atom_id.frag).
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
	uvec2 maskTable;        // GpuScene::maskTableAddress: the cut-outs' masks (JAH_MASKED; 0 otherwise)
	uvec2 clusterIds;       // GpuCull::clusterIds: the late pass's per-cluster draws' identity table (0 otherwise)
} pc;

layout( location = 0 ) flat out uint outSlot;
layout( location = 1 ) flat out uint outTriBase;
#ifdef JAH_MASKED
layout( location = 2 ) out vec2 outUv;
#endif

out gl_PerVertex
{
	vec4 gl_Position;
};

void main()
{
	// THE LATE PASS'S PER-CLUSTER DRAWS (ATOM-8 stage 1): firstInstance is a rejected entry, and the identity table
	// gives its slot and the cluster's first stream triangle; every other draw's firstInstance is the slot.
	bool perCluster = ( pc.clusterIds.x | pc.clusterIds.y ) != 0u;
	uint entry = uint( gl_InstanceIndex );
	uint slot = perCluster ? AtomWordRef( pc.clusterIds ).v[entry * 2u] : entry;
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
	outTriBase = perCluster ? AtomWordRef( pc.clusterIds ).v[entry * 2u + 1u] : AtomWordRef( pc.slotBase ).v[slot];
#ifdef JAH_MASKED
	outUv = geomUv( row, uint( gl_VertexIndex ) );
#endif
}
