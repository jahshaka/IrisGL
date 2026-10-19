// Jahshaka — ATOM-2 stage 1: THE DENSITY RESOLVE, the 'density' Atom view's table.
//
// One thread a GPU scene slot: the BASE mesh's level-0 triangles (the mesh table's level-0 index count / 3) over the
// slot's visible Atom pixels this frame (JahAtomDensityCount_cs.glsl's 16 depth words summed), written as float bits
// into the view's table texture at the slot's texel (1024 slots a row, the Atom view's table layout); 0 where the
// slot has no pixel. The view's quad colours it (JahAtomView_ps.glsl, mode 5). No Hlms directive mark appears in any
// comment here.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

struct GpuMesh
{
	uvec4 counts;           // y the level-0 indices
	vec4  localBoundsMin;
	vec4  localBoundsMax;
	uvec4 dag;
};

layout( std430, ogre_U0 ) readonly restrict buffer countLayout { uint counts[]; };
// The instance table as raw vec4 LANES (GpuScene.h: 10 per entry; the mesh index bit-cast into lane 6's w).
layout( std430, ogre_U1 ) readonly restrict buffer instLayout { vec4 instanceWords[]; };
layout( std430, ogre_U2 ) readonly restrict buffer meshLayout { GpuMesh meshes[]; };
layout( vulkan( ogre_u3 ) vk_comma r32ui )
uniform restrict writeonly uimage2D densityTable;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_INSTANCE_LANES 10u

void main()
{
	uint slot = gl_GlobalInvocationID.x;
	ivec2 tSize = imageSize( densityTable );
	ivec2 txy = ivec2( int( slot & 1023u ), int( slot >> 10u ) );
	if( slot >= counts[0] || txy.y >= tSize.y )
		return;
	uint pixels = 0u;
	for( uint d = 0u; d < 16u; ++d )
		pixels += counts[4u + slot * 16u + d];
	float density = 0.0;
	if( pixels != 0u )
	{
		uint meshIndex = floatBitsToUint( instanceWords[slot * JAH_INSTANCE_LANES + 6u].w );
		density = float( meshes[meshIndex].counts.y / 3u ) / float( pixels );
	}
	imageStore( densityTable, txy, uvec4( floatBitsToUint( density ), 0u, 0u, 0u ) );
}
