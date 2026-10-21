// Jahshaka — ATOM-2 stage 1: THE DENSITY RESOLVE, the 'density' Atom view's table.
//
// One thread a GPU scene slot: the triangles the CUT DREW for the object this frame, both lists (the count's slot word
// 16, JahAtomDensityDrawn_cs.glsl - ATOM-8 stage 0: the base mesh's level-0 size it divided before grew as the square of
// the distance whatever the cut did) over the slot's visible Atom pixels this frame (JahAtomDensityCount_cs.glsl's 16
// depth words summed), written as float bits into the view's table texture at the slot's texel (1024 slots a row, the
// Atom view's table layout); 0 where the slot has no pixel. The view's quad colours it (JahAtomView_ps.glsl, mode 5). No
// Hlms directive mark appears in any comment here.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

layout( std430, ogre_U0 ) readonly restrict buffer countLayout { uint counts[]; };
layout( vulkan( ogre_u1 ) vk_comma r32ui )
uniform restrict writeonly uimage2D densityTable;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_SLOT_WORDS 20u
#define JAH_COUNT_HEADER 4u

void main()
{
	uint slot = gl_GlobalInvocationID.x;
	ivec2 tSize = imageSize( densityTable );
	ivec2 txy = ivec2( int( slot & 1023u ), int( slot >> 10u ) );
	if( slot >= counts[0] || txy.y >= tSize.y )
		return;
	uint base = JAH_COUNT_HEADER + slot * JAH_SLOT_WORDS;
	uint pixels = 0u;
	for( uint d = 0u; d < 16u; ++d )
		pixels += counts[base + d];
	float density = 0.0;
	if( pixels != 0u )
		density = float( counts[base + 16u] ) / float( pixels );
	imageStore( densityTable, txy, uvec4( floatBitsToUint( density ), 0u, 0u, 0u ) );
}
