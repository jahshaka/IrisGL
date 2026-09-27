// ATOM-SHADOWS-1 — THE CASTER CUT'S VERTEX STAGE (OgreAtomCasterPass.cpp records it).
//
// A shadow map's Atom casters are drawn from the LIGHT'S OWN CLUSTER CUT: the cull's
// cut mode run with the shadow camera (JahCullCut_cs.glsl, the view's rule at the
// map's texel), its compacted stream the index buffer and one indirect command per
// surviving instance (firstInstance = the instance slot) — the id pass's shape, depth
// only. Every vertex is pulled through the mesh's CLUSTER ROW exactly as atom_id.vert
// pulls it (jah_geom_rows.glsl, the one decode), and transformed in the order the
// stock PBS caster transforms it: the instance's world rows, then the pass's
// view-projection rows (HlmsPbs::preparePassHash's matrix, the texture flip included).
//
// THE CONSTANT BIAS is the stock caster's (ShadowCaster_piece_vs.any, the reverse-Z
// branches): the datablock's mShadowConstantBias (the instance's raster.w, bit-cast)
// x the shadow camera's constant-bias scale x the pass's depth range — subtracted from
// clip z for a directional or spot map, ADDED for a point map, whose fragment stage
// (atom_caster_point.frag) also writes the biased linear distance into the colour.
#version 460
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require
#extension GL_GOOGLE_include_directive : require

#include "jah_geom_rows.glsl"

layout( buffer_reference, std430, buffer_reference_align = 16 ) readonly buffer AtomVec4Ref
{
	uvec4 v[];
};

// 128 bytes: the guaranteed push-constant size, exactly (OgreAtomCasterPass.cpp's
// CasterPushConstants mirrors it).
layout( push_constant ) uniform AtomCasterPc
{
	vec4  viewProjRow[4];   // row i of proj * view, the pass's own (texture flip included)
	vec4  eyeBias;          // xyz the shadow camera's position (point maps), w its constant-bias scale
	vec4  depthRange;       // x near, y 1 / (far - near), z 1 = a point map, w unused
	uvec2 instances;        // GpuScene::instanceBuffer: 10 uvec4 per GpuInstance
	uvec2 meshes;           // GpuScene::meshBuffer: 4 uvec4 per GpuMesh (dag = lane 3)
	uvec2 rows;             // GpuScene::geomBuffer: 3 uvec4 per geometry row
	uvec2 pad;
} pc;

layout( location = 0 ) out vec3 outToCamera;
layout( location = 1 ) flat out float outConstBias;

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
	vec4 clip = vec4( dot( pc.viewProjRow[0], w4 ), dot( pc.viewProjRow[1], w4 ),
					  dot( pc.viewProjRow[2], w4 ), dot( pc.viewProjRow[3], w4 ) );

	// raster.w: the datablock's constant bias (GpuInstance::raster).
	float bias = uintBitsToFloat( inst.v[slot * 10u + 9u].w ) * pc.eyeBias.w;
	bool point = pc.depthRange.z != 0.0;
	float constBias = ( point ? bias : -bias ) * pc.depthRange.y;
	clip.z = clip.z + constBias;
	gl_Position = clip;

	outToCamera = w - pc.eyeBias.xyz;
	outConstBias = constBias;
}
