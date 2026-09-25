// ATOM S3-DRAW — THE ID PASS'S VERTEX STAGE (OgreAtomIdPass.cpp records it).
//
// No vertex attributes: every vertex is PULLED through the GPU scene's tables, the
// same rows the decode and the voxeliser read (jah_geom_rows.glsl, the ONE copy of
// the geometry-row decode). The draw is one VkDrawIndexedIndirectCommand of the
// GPU cull's list (JahCullDraws_cs.glsl): firstInstance is the INSTANCE SLOT, and
// firstIndex/indexCount are the chosen level's range, drawn over an IDENTITY index
// buffer so gl_VertexIndex is the index ELEMENT in the level's own index buffer
// (the meshes live in different index buffers; one indirect draw binds one).
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
	uvec2 levels;           // GpuScene::levelBuffer: 2 uvec4 per GpuMeshLevel
	uvec2 rows;             // GpuScene::geomBuffer: 3 uvec4 per geometry row
	uvec2 cullLevels;       // GpuCull::levels: one uint per slot, the level the cull chose
} pc;

layout( location = 0 ) flat out uint outSlotLevel;
layout( location = 1 ) flat out uint outTriangle;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main()
{
	uint slot = uint( gl_InstanceIndex );
	AtomVec4Ref inst = AtomVec4Ref( pc.instances );
	uint mesh = inst.v[slot * 10u + 6u].w;
	uint level = AtomWordRef( pc.cullLevels ).v[slot] & 0x7u;
	uvec4 lev = AtomVec4Ref( pc.levels ).v[( mesh * 8u + level ) * 2u];

	AtomVec4Ref rows = AtomVec4Ref( pc.rows );
	GeometryRow row;
	row.addresses = rows.v[lev.w * 3u];
	row.layout0 = rows.v[lev.w * 3u + 1u];
	row.layout1 = rows.v[lev.w * 3u + 2u];

	uint element = uint( gl_VertexIndex );
	vec4 p = vec4( geomPosition( row, geomIndex( row, element ) ), 1.0 );
	vec3 w = vec3( dot( uintBitsToFloat( inst.v[slot * 10u + 0u] ), p ),
				   dot( uintBitsToFloat( inst.v[slot * 10u + 1u] ), p ),
				   dot( uintBitsToFloat( inst.v[slot * 10u + 2u] ), p ) );
	vec4 w4 = vec4( w, 1.0 );
	gl_Position = vec4( dot( pc.viewProjRow[0], w4 ), dot( pc.viewProjRow[1], w4 ),
						dot( pc.viewProjRow[2], w4 ), dot( pc.viewProjRow[3], w4 ) );

	// AtomId: x = slot (24 bits) | level (3 bits); y = the triangle counted from the
	// level's first index. Flat, so the PROVOKING vertex (the triangle's first)
	// decides: its element is firstIndex + 3t.
	outSlotLevel = ( slot & 0x00FFFFFFu ) | ( level << 24u );
	outTriangle = ( element - lev.x ) / 3u;
}
