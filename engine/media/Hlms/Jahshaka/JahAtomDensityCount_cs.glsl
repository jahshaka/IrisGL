// Jahshaka — ATOM-2 stage 1: THE DENSITY COUNT, one frame's visible Atom pixels per (object, depth).
//
// Over the view's id image after the post chain (the Atom view's own point, OgreAtomDraw.cpp's listener): a
// pixel counts where the id names an Atom item AND the scene's final depth is the id pass's depth - exactly the
// pixels the Atom view paints (JahAtomView_ps.glsl's test) - under the key slot x 16 + the DAG depth the cut drew it
// at (15 = 15 and deeper). The counts buffer is zeroed by the host before the dispatch; one workgroup is an 8 x 8
// tile, and a tile adds each key it holds ONCE (the first lane holding the key adds the tile's whole count), so a
// wall filling the view costs one global atomic per tile, not one per pixel.
//
// Read by: the density resolve (JahAtomDensityResolve_cs.glsl, the view's colours) and the host's measuring reads
// (Scene::atomCutProbe, AtomDrawStatus's density figures). No Hlms directive mark appears in any comment here.
@insertpiece( SetCrossPlatformSettings )

vulkan_layout( ogre_t0 ) uniform utexture2D atomIds;
vulkan_layout( ogre_t1 ) uniform texture2D atomIdDepth;
vulkan_layout( ogre_t2 ) uniform texture2D sceneDepth;

// [0] the slot capacity the buffer was sized for, [1..3] zero, then 16 words a slot.
layout( std430, ogre_U0 ) restrict buffer countLayout { uint counts[]; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_TILE ( @value( threads_per_group_x )u * @value( threads_per_group_y )u )
shared uint sKey[JAH_TILE];

void main()
{
	ivec2 size = textureSize( atomIds, 0 );
	ivec2 xy = ivec2( gl_GlobalInvocationID.xy );
	uint lane = gl_LocalInvocationIndex;
	uint key = 0xFFFFFFFFu;
	if( xy.x < size.x && xy.y < size.y )
	{
		uvec2 id = texelFetch( atomIds, xy, 0 ).xy;
		ivec2 dSize = textureSize( atomIdDepth, 0 );
		ivec2 sSize = textureSize( sceneDepth, 0 );
		float idDepth = texelFetch( atomIdDepth, clamp( xy, ivec2( 0 ), dSize - ivec2( 1 ) ), 0 ).x;
		float finalDepth = texelFetch( sceneDepth, clamp( xy, ivec2( 0 ), sSize - ivec2( 1 ) ), 0 ).x;
		uint slot = id.x & 0x00FFFFFFu;
		if( id.x != 0xFFFFFFFFu && floatBitsToUint( idDepth ) == floatBitsToUint( finalDepth ) && slot < counts[0] )
			key = slot * 16u + min( id.x >> 24u, 15u );
	}
	sKey[lane] = key;
	barrier();
	if( key != 0xFFFFFFFFu )
	{
		uint n = 0u;
		bool first = true;
		for( uint j = 0u; j < JAH_TILE; ++j )
		{
			if( sKey[j] == key )
			{
				++n;
				if( j < lane )
					first = false;
			}
		}
		if( first )
			atomicAdd( counts[4u + key], n );
	}
}
