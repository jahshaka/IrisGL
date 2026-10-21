// Jahshaka — ATOM-2 stage 1: THE DENSITY COUNT, one frame's visible Atom pixels per (object, depth).
//
// Over the view's id image after the post chain (the Atom view's own point, OgreAtomDraw.cpp's listener): a
// pixel counts where the id names an Atom item AND the scene's final depth is the id pass's depth - exactly the
// pixels the Atom view paints (JahAtomView_ps.glsl's test) - under the key slot x JAH_SLOT_WORDS + the DAG depth the
// cut drew it at (15 = 15 and deeper). The counts buffer is zeroed by the host before the dispatch; one workgroup is
// an 8 x 8 tile, and a tile adds each key it holds ONCE (the first lane holding the key adds the tile's whole count),
// so a wall filling the view costs one global atomic per tile, not one per pixel.
//
// THE VISIBLE CLUSTERS (ATOM-8 stage 0, the probe's frame only: counts[1] = the bitmap's bits, 0 otherwise): every
// counted pixel also sets the bit of its (slot, cluster) pair - clusterBase[slot] (the host's prefix of the slots'
// DAG cluster counts) + the cluster's index in its mesh's DAG (the id's y word, bits 8 and up) - once per tile.
// One bit per INSTANCE'S cluster, never per mesh's: two copies of one mesh are two objects to the reader.
// The drawn half (JahAtomDensityDrawn_cs.glsl) reads the bits back through the cut's records.
//
// Read by: the drawn reduce, the density resolve (JahAtomDensityResolve_cs.glsl, the view's colours) and the
// host's measuring reads (Scene::atomCutProbe, AtomDrawStatus's density and hidden figures). No Hlms directive mark
// appears in any comment here.
@insertpiece( SetCrossPlatformSettings )

vulkan_layout( ogre_t0 ) uniform utexture2D atomIds;
vulkan_layout( ogre_t1 ) uniform texture2D atomIdDepth;
vulkan_layout( ogre_t2 ) uniform texture2D sceneDepth;

// THE COUNT WORDS (OgreAtomDensity.cpp kCountHeader / kSlotWords): [0] the slot capacity the buffer was sized for,
// [1] the visible-cluster bitmap's bits (0 = no bitmap this frame), [2] the triangles drawn by slots with no visible
// pixel, [3] every triangle drawn; then JAH_SLOT_WORDS a slot: 0-15 the pixels by depth, 16 the triangles the cut
// drew, 17 its drawn clusters, 18 its visible clusters (the probe's frame), 19 zero.
layout( std430, ogre_U0 ) restrict buffer countLayout { uint counts[]; };
layout( std430, ogre_U1 ) readonly restrict buffer clusterBaseLayout { uint clusterBase[]; };
layout( std430, ogre_U2 ) restrict buffer seenLayout { uint seen[]; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_TILE ( @value( threads_per_group_x )u * @value( threads_per_group_y )u )
#define JAH_SLOT_WORDS 20u
shared uint sKey[JAH_TILE];
shared uint sBit[JAH_TILE];

void main()
{
	ivec2 size = textureSize( atomIds, 0 );
	ivec2 xy = ivec2( gl_GlobalInvocationID.xy );
	uint lane = gl_LocalInvocationIndex;
	uint key = 0xFFFFFFFFu;
	uint bit = 0xFFFFFFFFu;
	if( xy.x < size.x && xy.y < size.y )
	{
		uvec2 id = texelFetch( atomIds, xy, 0 ).xy;
		ivec2 dSize = textureSize( atomIdDepth, 0 );
		ivec2 sSize = textureSize( sceneDepth, 0 );
		float idDepth = texelFetch( atomIdDepth, clamp( xy, ivec2( 0 ), dSize - ivec2( 1 ) ), 0 ).x;
		float finalDepth = texelFetch( sceneDepth, clamp( xy, ivec2( 0 ), sSize - ivec2( 1 ) ), 0 ).x;
		uint slot = id.x & 0x00FFFFFFu;
		if( id.x != 0xFFFFFFFFu && floatBitsToUint( idDepth ) == floatBitsToUint( finalDepth ) && slot < counts[0] )
		{
			key = slot * JAH_SLOT_WORDS + min( id.x >> 24u, 15u );
			if( counts[1] != 0u )
			{
				// the base buffer holds a slot's word only in the bitmap's frame (sized to the slot capacity then)
				uint b = clusterBase[slot] + ( id.y >> 8u );
				if( b < counts[1] )
					bit = b;
			}
		}
	}
	sKey[lane] = key;
	sBit[lane] = bit;
	barrier();
	if( bit != 0xFFFFFFFFu )
	{
		bool firstBit = true;
		for( uint j = 0u; j < lane && firstBit; ++j )
			firstBit = sBit[j] != bit;
		if( firstBit )
			atomicOr( seen[bit >> 5u], 1u << ( bit & 31u ) );
	}
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
